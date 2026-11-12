// Shared world-lighting record codecs. Mirrors the C++ PackedInterval,
// PackedCH, SourceMaterial and resolved-cell words byte for byte. Every
// component is extracted from explicit integer words: no native half3, int8
// vector or struct-layout assumption is part of this ABI.
#ifndef PPR_WORLD_LIGHTING_CODEC_SLANG
#define PPR_WORLD_LIGHTING_CODEC_SLANG

// ------------------------------------------------------------------
// published record shapes (byte-identical to the C++ records)
// ------------------------------------------------------------------

struct PackedInterval {
    uint m_rg; // word 0: FP16 radiance R in bits 0-15, G in bits 16-31
    uint m_bt; // word 1: FP16 radiance B in bits 0-15, transmittance in bits 16-31
};

struct PackedCH {
    uint m_dc;       // word 0: RGB9E5 C0 coefficient
    float m_scale;   // word 1: FP32 shared AC quantization scale
    uint m_ac[6];    // words 2-7: eight signed int8 RGB triplets, four bytes per word
};

struct SourceMaterial {
    uint m_emission; // RGB9E5 authored emission with strength already multiplied
    uint m_diffuse;  // RGB9E5 diffuse reflectance
};

struct Interval {
    float3 m_radiance;
    float m_transmittance;
};

struct CH {
    float3 m_coefficients[9];
};

// ------------------------------------------------------------------
// binary16 decode
// ------------------------------------------------------------------

// Exact IEEE binary16 to binary32 conversion. Half-subnormal inputs are
// normalized from their ten-bit significand; zero preserves its sign bit.
float unpackHalf(uint bits) {
    uint sign = (bits & 0x8000u) != 0u ? 0x80000000u : 0u;
    uint exponent = (bits >> 10u) & 0x1fu;
    uint mantissa = bits & 0x3ffu;

    if (exponent == 0u) {
        if (mantissa == 0u) return asfloat(sign);

        uint shift = 0u;
        while ((mantissa & 0x400u) == 0u) {
            mantissa <<= 1u;
            ++shift;
        }
        mantissa &= 0x3ffu;
        return asfloat(sign | ((113u - shift) << 23u) | (mantissa << 13u));
    }

    if (exponent == 31u) return asfloat(sign | 0x7f800000u | (mantissa << 13u));

    return asfloat(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
}

// ------------------------------------------------------------------
// binary16 encode
// ------------------------------------------------------------------

// Exact IEEE binary32 -> binary16 nearest/ties-to-even, mirroring the host
// codec. Interpreted values are the validated nonnegative coarse radiance and
// transmittance words; the binary32 subnormal range is below binary16's
// smallest subnormal and encodes as zero.
uint packHalf(float value) {
    uint bits = asuint(value);
    uint sign = (bits >> 16u) & 0x8000u;
    uint exponent = (bits >> 23u) & 0xffu;
    uint mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu) return sign | 0x7c00u | (mantissa != 0u ? 0x200u : 0u);
    if (exponent == 0u) return sign;

    int half_exponent = int(exponent) - 112;
    if (half_exponent >= 0x1f) return sign | 0x7c00u;

    if (half_exponent <= 0) {
        // Binary16 subnormal: value = significand * 2^(exponent-126), so the
        // 24-bit significand shifts down by 126-exponent with guard/round/
        // sticky handled by one retained bit and the discarded remainder.
        uint shift = uint(126 - int(exponent));
        if (shift > 24u) return sign;

        uint significand = mantissa | 0x800000u;
        uint retained = significand >> shift;
        uint remainder = significand & ((1u << shift) - 1u);
        uint halfway = 1u << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (retained & 1u) != 0u)) ++retained;
        return sign | retained;
    }

    uint retained = mantissa >> 13u;
    uint remainder = mantissa & 0x1fffu;
    uint result = sign | (uint(half_exponent) << 10u) | retained;
    // A carry here propagates into the exponent field, which is the correct
    // nearest encoding of a value that rounds up to the next binade.
    if (remainder > 0x1000u || (remainder == 0x1000u && (retained & 1u) != 0u)) ++result;
    return result;
}

// ------------------------------------------------------------------
// shared-exponent RGB9E5 decode
// ------------------------------------------------------------------

// Mantissas occupy bits 0..8/9..17/18..26 and the shared exponent bits
// 27..31. Decode is mantissa*2^(E-24); every factor is an exact power of two.
float3 unpackRGB9E5(uint word) {
    uint exponent = (word >> 27u) & 0x1fu;
    float scale = exp2(float(exponent) - 24.0);
    return float3(
               float(word & 0x1ffu),
               float((word >> 9u) & 0x1ffu),
               float((word >> 18u) & 0x1ffu)) *
           scale;
}

// ------------------------------------------------------------------
// coarse interval, fine CH, source material and resolved cell decode
// ------------------------------------------------------------------

Interval unpackInterval(PackedInterval value) {
    Interval result;
    result.m_radiance = float3(
        unpackHalf(value.m_rg & 0xffffu),
        unpackHalf((value.m_rg >> 16u) & 0xffffu),
        unpackHalf(value.m_bt & 0xffffu));
    result.m_transmittance = unpackHalf((value.m_bt >> 16u) & 0xffffu);
    return result;
}

// One component of the 24 AC bytes. Component index 0 is the first component
// of cos1; the first component is the least-significant byte of its word.
float unpackCHComponent(PackedCH value, uint component) {
    uint word = value.m_ac[component >> 2u];
    uint byte = (word >> ((component & 3u) * 8u)) & 0xffu;
    return float(int(byte << 24u) >> 24) * value.m_scale;
}

CH unpackCH(PackedCH value) {
    CH result;
    result.m_coefficients[0] = unpackRGB9E5(value.m_dc);
    for (uint coefficient = 0u; coefficient < 8u; ++coefficient) {
        uint base = coefficient * 3u;
        result.m_coefficients[coefficient + 1u] = float3(
            unpackCHComponent(value, base),
            unpackCHComponent(value, base + 1u),
            unpackCHComponent(value, base + 2u));
    }
    return result;
}

// ------------------------------------------------------------------
// fine CH encode
// ------------------------------------------------------------------
// Mirrors the host codec: the DC coefficient is nonnegative RGB9E5, AC is one
// signed eight-bit component per RGB channel (range [-127, 127], never -128)
// with one shared FP32 scale = max(abs(AC))/127. Zero AC writes scale zero and
// all 24 AC bytes zero. Quantization is nearest/ties-to-even on the clamped
// endpoint.
uint chPackCategory(CH value) {
    if (!all(isfinite(value.m_coefficients[0])) || any(value.m_coefficients[0] < 0.0)) return 1u;
    if (any(value.m_coefficients[0] > 65408.0)) return 2u;

    for (uint coefficient = 1u; coefficient < 9u; ++coefficient) {
        if (!all(isfinite(value.m_coefficients[coefficient]))) return 1u;
    }

    return 0u;
}

PackedCH packCH(CH value) {
    PackedCH packed;
    packed.m_dc = packRGB9E5(value.m_coefficients[0]);
    packed.m_scale = 0.0;
    packed.m_ac[0] = 0u;
    packed.m_ac[1] = 0u;
    packed.m_ac[2] = 0u;
    packed.m_ac[3] = 0u;
    packed.m_ac[4] = 0u;
    packed.m_ac[5] = 0u;

    float maximum = 0.0;
    for (uint coefficient = 1u; coefficient < 9u; ++coefficient) {
        maximum = max(maximum, max(abs(value.m_coefficients[coefficient].x),
                                  max(abs(value.m_coefficients[coefficient].y),
                                      abs(value.m_coefficients[coefficient].z))));
    }

    if (maximum == 0.0) return packed;

    float scale = maximum / 127.0;
    // A nonzero maximum whose FP32 scale underflows encodes zero AC; its
    // absolute error is below 127 times the smallest positive FP32 value.
    if (scale == 0.0) return packed;
    packed.m_scale = scale;

    for (uint coefficient = 1u; coefficient < 9u; ++coefficient) {
        for (uint channel = 0u; channel < 3u; ++channel) {
            float component = channel == 0u ? value.m_coefficients[coefficient].x
                              : channel == 1u ? value.m_coefficients[coefficient].y
                                              : value.m_coefficients[coefficient].z;
            float scaled = component / scale;
            float base = floor(scaled);
            float fraction = scaled - base;
            int quantized = int(base);
            if (fraction > 0.5 || (fraction == 0.5 && (quantized & 1) != 0)) ++quantized;
            quantized = clamp(quantized, -127, 127);

            uint packed_component = (coefficient - 1u) * 3u + channel;
            packed.m_ac[packed_component >> 2u] |=
                (uint(quantized) & 0xffu) << ((packed_component & 3u) * 8u);
        }
    }

    return packed;
}

float3 unpackSourceEmission(SourceMaterial material) {
    return unpackRGB9E5(material.m_emission);
}

float3 unpackSourceDiffuse(SourceMaterial material) {
    return unpackRGB9E5(material.m_diffuse);
}

// Resolved cell: material ID in bits 0-15, IEEE binary16 Kelvin in 16-31.
// Material ID zero is the empty cell.
uint unpackCellMaterial(uint cell) {
    return cell & 0xffffu;
}

float unpackCellTemperature(uint cell) {
    return unpackHalf((cell >> 16u) & 0xffffu);
}

// ------------------------------------------------------------------
// shared-exponent RGB9E5 encode
// ------------------------------------------------------------------
// Mirrors the host codec: validate the radiance domain, encode an all-zero
// triple as word zero, then pick the smallest shared exponent whose rounded
// mantissas fit in [0, 511] with nearest/ties-up rounding.
uint rgb9e5PackCategory(float3 rgb) {
    if (!all(isfinite(rgb)) || any(rgb < 0.0)) return 1u;
    if (any(rgb > 65408.0)) return 2u;
    return 0u;
}

uint packRGB9E5(float3 rgb) {
    if (rgb.x == 0.0 && rgb.y == 0.0 && rgb.z == 0.0) return 0u;

    for (uint exponent = 0u; exponent <= 31u; ++exponent) {
        float3 scaled = rgb * exp2(24.0 - float(exponent));
        // floor(scaled + 0.5) > 511 exactly when scaled >= 511.5.
        if (any(scaled >= 511.5)) continue;

        uint3 mantissa = uint3(floor(scaled + 0.5));
        return mantissa.x | (mantissa.y << 9u) | (mantissa.z << 18u) | (exponent << 27u);
    }

    return 0u;
}

// ------------------------------------------------------------------
// coarse interval encode
// ------------------------------------------------------------------
// Category 0 is encodable, 1 is invalid arithmetic/state (nonfinite, negative
// radiance or a transmittance outside [0,1]) and 2 is representational overflow.
uint intervalPackCategory(Interval value) {
    if (!all(isfinite(value.m_radiance)) || any(value.m_radiance < 0.0)) return 1u;
    if (!isfinite(value.m_transmittance) || value.m_transmittance < 0.0 || value.m_transmittance > 1.0) return 1u;
    if (any(value.m_radiance > 65504.0)) return 2u;
    return 0u;
}

PackedInterval packInterval(Interval value) {
    PackedInterval packed;
    packed.m_rg = packHalf(value.m_radiance.x) | (packHalf(value.m_radiance.y) << 16u);
    packed.m_bt = packHalf(value.m_radiance.z) | (packHalf(value.m_transmittance) << 16u);
    return packed;
}

// ------------------------------------------------------------------
// visible palette classification
// ------------------------------------------------------------------
// 96-byte GridTerrainVisual mirror. Acceleration and source preparation read
// the same authored classification; the projection consumes it through this
// definition too.
struct TerrainVisual {
    uint material;
    uint rgba;
    uint textureIndex;
    float texturePeriod;
    float roundness;
    float filteredMean[3];
    uint normalTextureIndex;
    float metallic;
    float roughness;
    float opacity;
    float emissive[3];
    float emissiveStrength;
    float ior;
    float normalScale;
    float refractionDepth;
    uint surfaceFlags;
    uint transportModel;
    float sigma[3];
};

static const uint kOpticalBlendSurfaceFlag = 1u;

// Occupancy is opaque classification, not brightness or a nonzero material ID:
// material zero is empty, a non-emissive receive-only (optical blend) surface is
// transparent, and every other resolved material is opaque regardless of its
// emission.
bool resolvedCellOccupied(uint resolved, TerrainVisual visual) {
    if ((resolved & 0xffffu) == 0u) return false;

    bool receiveOnly =
        (visual.surfaceFlags & kOpticalBlendSurfaceFlag) != 0u && visual.emissiveStrength == 0.0;
    return !receiveOnly;
}

#endif
