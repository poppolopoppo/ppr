// ------------------------------------------------------------------
// circular-harmonic basis, weighted fitting and angular mean
// ------------------------------------------------------------------
// Orthonormal circular basis approved for the fine CH field:
//
//   Y0       = 1/sqrt(2*pi)
//   Yc_m     = cos(m*theta)/sqrt(pi)
//   Ys_m     = sin(m*theta)/sqrt(pi), m=1..4
//   L(theta) = C0*Y0 + sum(Cc_m*Yc_m + Cs_m*Ys_m)
//
// Sample fitting normalizes the quadrature weight, subtracts the mean before
// the AC projection so sample density cannot leak into the harmonic, and
// applies the Lanczos window sinc(m*pi/5) exactly once. Loading, interpolating
// and blending decoded coefficients never re-apply the window.
//
// This include declares no resource of its own. The including module must
// declare its fine-CH binding named `g_fine_ch` before including this file:
// `StructuredBuffer<PackedCH>` for a scene/visible reader or
// `RWStructuredBuffer<PackedCH>` for the in-place writer. Only the field
// sampling helper below reads it.
#ifndef PPR_WORLD_LIGHTING_CH_SLANG
#define PPR_WORLD_LIGHTING_CH_SLANG

#include "world_lighting_codec.slang"

static const uint kFineCHEdge = 512u;
static const float kChTwoPi = 6.28318530717958647692;
static const float kChY0 = 0.39894228040143267794;      // 1/sqrt(2*pi)
static const float kChInvSqrtPi = 0.56418958354775628694; // 1/sqrt(pi)

// sinc(x) = sin(x)/x, with the continuous value one at the origin.
float chSinc(float x) {
    return x == 0.0 ? 1.0 : sin(x) / x;
}

// Lanczos window for harmonic order m, applied once when fitting fresh samples.
float chLanczos(uint m) {
    return chSinc(float(m) * 3.14159265358979323846 / 5.0);
}

// Streamed weighted accumulator. The first sample's radiance is the reference
// for a relative mean, so identical samples produce exactly zero AC instead of
// cancellation noise. Mathematically this is the specified
//   mean = sum(w*L)/W
//   Cc_m = sinc(m*pi/5) * sum(q*(L-mean)*cos(m*theta))/sqrt(pi)
// with q = w*2*pi/W, expanded as
//   (2*pi/W) * (sum(w*(L-ref)*cos) - (mean-ref)*sum(w*cos)).
struct CHAccumulator {
    float m_weight;
    float3 m_reference;
    bool m_has_reference;
    float3 m_relative;
    float3 m_cos[4];
    float3 m_sin[4];
    float m_cos_weight[4];
    float m_sin_weight[4];
};

CHAccumulator chAccumulator() {
    CHAccumulator result;
    result.m_weight = 0.0;
    result.m_reference = float3(0.0);
    result.m_has_reference = false;
    result.m_relative = float3(0.0);
    for (uint m = 0u; m < 4u; ++m) {
        result.m_cos[m] = float3(0.0);
        result.m_sin[m] = float3(0.0);
        result.m_cos_weight[m] = 0.0;
        result.m_sin_weight[m] = 0.0;
    }

    return result;
}

void chAccumulate(inout CHAccumulator accumulator, float3 radiance, float theta, float weight) {
    if (!accumulator.m_has_reference) {
        accumulator.m_reference = radiance;
        accumulator.m_has_reference = true;
    }
    float3 relative = radiance - accumulator.m_reference;

    accumulator.m_weight += weight;
    accumulator.m_relative += relative * weight;

    for (uint m = 1u; m <= 4u; ++m) {
        float cosine = cos(float(m) * theta);
        float sine = sin(float(m) * theta);
        accumulator.m_cos[m - 1u] += relative * (weight * cosine);
        accumulator.m_sin[m - 1u] += relative * (weight * sine);
        accumulator.m_cos_weight[m - 1u] += weight * cosine;
        accumulator.m_sin_weight[m - 1u] += weight * sine;
    }
}

// Direction-based form of chAccumulate for a caller that already holds the unit
// connector basis (cos theta, sin theta). Order one is the basis pair itself and
// every higher order follows the complex-angle recurrence
//   (c, s)_m = (c, s)_{m-1} * (c1, s1),
// with both next values formed from the same previous pair, so a fit needs no
// atan2 and no harmonic sin/cos. Reference selection, relative-radiance
// subtraction, weight sums, accumulation order and chFinish are unchanged; the
// theta helper above stays the independent reference used by the probe corpus.
void chAccumulateDirection(inout CHAccumulator accumulator, float3 radiance, float2 basis, float weight) {
    if (!accumulator.m_has_reference) {
        accumulator.m_reference = radiance;
        accumulator.m_has_reference = true;
    }
    float3 relative = radiance - accumulator.m_reference;

    accumulator.m_weight += weight;
    accumulator.m_relative += relative * weight;

    float cosine = basis.x;
    float sine = basis.y;
    for (uint m = 1u; m <= 4u; ++m) {
        accumulator.m_cos[m - 1u] += relative * (weight * cosine);
        accumulator.m_sin[m - 1u] += relative * (weight * sine);
        accumulator.m_cos_weight[m - 1u] += weight * cosine;
        accumulator.m_sin_weight[m - 1u] += weight * sine;

        // Both next-order values read the previous pair, never each other.
        float next_cosine = cosine * basis.x - sine * basis.y;
        float next_sine = sine * basis.x + cosine * basis.y;
        cosine = next_cosine;
        sine = next_sine;
    }
}

// Finishes the fit. A zero or nonfinite total weight is a numeric failure, not
// a black substitute; the caller reports its category and writes containment.
bool chFinish(CHAccumulator accumulator, out CH result) {
    float total = accumulator.m_weight;
    if (!(total > 0.0) || !isfinite(total)) {
        // Numeric failure: the caller reports its category and writes
        // containment instead of publishing this value. Keep the out parameter
        // initialized on that path so no caller can observe indeterminate data.
        for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
            result.m_coefficients[coefficient] = float3(0.0);
        }
        return false;
    }

    float3 relative_mean = accumulator.m_relative / total;
    float3 mean = accumulator.m_reference + relative_mean;
    result.m_coefficients[0] = mean / kChY0;

    float quadrature = kChTwoPi / total;
    for (uint m = 1u; m <= 4u; ++m) {
        float3 cosine_sum =
            (accumulator.m_cos[m - 1u] - relative_mean * accumulator.m_cos_weight[m - 1u]) *
            (quadrature * chLanczos(m) * kChInvSqrtPi);
        float3 sine_sum =
            (accumulator.m_sin[m - 1u] - relative_mean * accumulator.m_sin_weight[m - 1u]) *
            (quadrature * chLanczos(m) * kChInvSqrtPi);
        result.m_coefficients[2u * m - 1u] = cosine_sum;
        result.m_coefficients[2u * m] = sine_sum;
    }

    return true;
}

// Fine-probe field sample at a continuous world-cell position. Fine probe
// centres sit at (probe+0.5)*2 native cells, so the probe coordinate is
// position/2-0.5. Decoded DC is interpolated in FP32 with ordinary bilinear
// weights and clamped edge indices (the weights still sum to one), then scaled
// by Y0. No additional pi, cosine, normal, roughness or Fresnel factor applies.
float3 angularMean(float2 world_cell_position) {
    float2 fine = world_cell_position * 0.5 - 0.5;
    int2 base = int2(floor(fine));
    float2 fraction = fine - float2(base);
    int2 last = int2(int(kFineCHEdge) - 1);

    float3 result = float3(0.0);
    for (uint tap = 0u; tap < 4u; ++tap) {
        int2 offset = int2(int(tap % 2u), int(tap / 2u));
        float weight =
            (offset.x == 0 ? 1.0 - fraction.x : fraction.x) * (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
        if (weight <= 0.0) continue;

        int2 index = clamp(base + offset, int2(0), last);
        PackedCH record = g_fine_ch[index.y * int(kFineCHEdge) + index.x];
        result += unpackRGB9E5(record.m_dc) * weight;
    }

    return result * kChY0;
}

#endif
