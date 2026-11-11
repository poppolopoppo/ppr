// ------------------------------------------------------------------
// isotropic VNDF/Smith BRDF integration table
// ------------------------------------------------------------------
// One immutable 64 (NoV) x 128 (roughness) RG16F texel pair per device:
//   .x = integral of Fresnel-weighted visibility (Fc_weighted_visibility)
//   .y = integral of unweighted visibility (unweighted_visibility)
// Texel centres evaluate NoV=(x+0.5)/64 and roughness=(y+0.5)/128 with 1,024
// golden-ratio/radical-inverse samples. Microfacet alpha is max(r, 0.001), not
// r squared, matching the isotropic slice of the approved anisotropic
// integration with anisotropy zero. Sampling is bilinear with clamp-to-edge.
#ifndef PPR_MATERIAL_BRDF_LUT_SLANG
#define PPR_MATERIAL_BRDF_LUT_SLANG

static const uint kBrdfLutWidth = 64u;
static const uint kBrdfLutHeight = 128u;
static const uint kBrdfLutSampleCount = 1024u;
static const float kBrdfMinAlpha = 0.001;
static const float kBrdfTwoPi = 6.28318530717958647692;

// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
float brdfRadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Golden-ratio/radical-inverse sample pair, exactly 0..1.
float2 brdfHammersley(uint index) {
    return frac(float2(float(index) * 0.6180339887, brdfRadicalInverse(index)));
}

float3 brdfNormalizeSafe(float3 value, float3 fallback) {
    float squared = dot(value, value);
    return squared > 1.0e-12 ? value * rsqrt(squared) : fallback;
}

// Isotropic slice of the VNDF half-vector sampler: the view vector is stretched
// by alpha in both tangent axes before the unit-disk hemisphere projection.
float3 brdfVndfSample(float2 xi, float alpha, float3 view) {
    float3 stretched = brdfNormalizeSafe(float3(alpha * view.x, alpha * view.y, view.z), float3(0.0, 0.0, 1.0));
    float3 tangent = stretched.z < 0.999
                         ? brdfNormalizeSafe(cross(stretched, float3(0.0, 0.0, 1.0)), float3(1.0, 0.0, 0.0))
                         : float3(1.0, 0.0, 0.0);
    float3 bitangent = cross(tangent, stretched);

    float radius = sqrt(max(xi.x, 0.0));
    float phi = kBrdfTwoPi * xi.y;
    float t1 = radius * cos(phi);
    float t2 = radius * sin(phi);
    float projected = 0.5 * (1.0 + stretched.z);
    t2 = (1.0 - projected) * sqrt(max(1.0 - t1 * t1, 0.0)) + projected * t2;

    float3 hemisphere = t1 * tangent + t2 * bitangent +
                        sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * stretched;
    return brdfNormalizeSafe(float3(alpha * hemisphere.x, alpha * hemisphere.y, max(0.0, hemisphere.z)),
        float3(0.0, 0.0, 1.0));
}

// Correlated Smith visibility G2 / (4 * NoV * NoL).
float brdfSmithVisibility(float alpha, float3 view, float3 light, float n_dot_v, float n_dot_l) {
    float lambda_v = n_dot_l * length(float3(alpha * view.x, alpha * view.y, n_dot_v));
    float lambda_l = n_dot_v * length(float3(alpha * light.x, alpha * light.y, n_dot_l));
    float denominator = lambda_v + lambda_l;
    return denominator > 1.0e-8 ? 0.5 / denominator : 0.0;
}

// Smith masking G1 for the view direction.
float brdfSmithG1(float alpha, float3 view, float n_dot_v) {
    float stretch = length(float3(alpha * view.x, alpha * view.y, n_dot_v));
    float denominator = n_dot_v + stretch;
    return denominator > 1.0e-8 ? 2.0 * n_dot_v / denominator : 0.0;
}

float brdfPow5(float value) {
    value = max(value, 0.0);
    float squared = value * value;
    return squared * squared * value;
}

// Returns (Fc-weighted visibility, unweighted visibility) for one texel.
float2 brdfIntegrateTexel(float n_dot_v, float roughness) {
    n_dot_v = max(n_dot_v, 1.0e-5);
    float alpha = max(roughness, kBrdfMinAlpha);
    float3 view = float3(sqrt(max(1.0 - n_dot_v * n_dot_v, 0.0)), 0.0, n_dot_v);

    float weighted = 0.0;
    float unweighted = 0.0;
    for (uint index = 0u; index < kBrdfLutSampleCount; ++index) {
        float2 xi = brdfHammersley(index);
        float3 half_vector = brdfVndfSample(xi, alpha, view);
        float3 light = brdfNormalizeSafe(reflect(-view, half_vector), half_vector);

        float n_dot_l = light.z;
        float v_dot_h = saturate(dot(view, half_vector));
        if (n_dot_l <= 0.0) continue;

        float visibility =
            brdfSmithVisibility(alpha, view, light, n_dot_v, n_dot_l);
        float g1 = brdfSmithG1(alpha, view, n_dot_v);
        float weight = g1 > 0.0 ? 4.0 * n_dot_l * visibility * n_dot_v / g1 : 0.0;
        float fresnel = brdfPow5(1.0 - v_dot_h);
        weighted += fresnel * weight;
        unweighted += weight;
    }

    float inverse_samples = 1.0 / float(kBrdfLutSampleCount);
    return float2(weighted, unweighted) * inverse_samples;
}

RWTexture2D<float2> g_brdf_lut;

[shader("compute")]
[numthreads(8, 8, 1)]
void integrateBrdfLut(uint2 texel : SV_DispatchThreadID) {
    if (texel.x >= kBrdfLutWidth || texel.y >= kBrdfLutHeight) return;

    float n_dot_v = (float(texel.x) + 0.5) / float(kBrdfLutWidth);
    float roughness = (float(texel.y) + 0.5) / float(kBrdfLutHeight);
    g_brdf_lut[texel] = brdfIntegrateTexel(n_dot_v, roughness);
}

#endif
