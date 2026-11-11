// Shared world-lighting record codecs. Mirrors the C++ PackedInterval,
// PackedCH, SourceMaterial and resolved-cell words byte for byte. Every
// component is extracted from explicit integer words: no native half3, int8
// vector or struct-layout assumption is part of this ABI.

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
