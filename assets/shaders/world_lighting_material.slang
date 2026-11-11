// ------------------------------------------------------------------
// visible directional material response
// ------------------------------------------------------------------
// The visible scene reuses the fine circular-harmonic field for directional
// diffuse irradiance and roughness-dependent specular radiance. These are
// approximate circular filters over the decoded coefficients, not a
// reconstruction of a full three-dimensional GGX environment. Decode and
// interpolate the coefficients at the shading location before calling these
// helpers.
#ifndef PPR_WORLD_LIGHTING_MATERIAL_SLANG
#define PPR_WORLD_LIGHTING_MATERIAL_SLANG

#include "world_lighting_codec.slang"

static const float kMaterialPi = 3.14159265358979323846;
static const float kMaterialInvSqrtPi = 0.56418958354775628694;
static const float kMaterialInvSqrtTwoPi = 0.39894228040143267794;

// x^5 with negative inputs contained at zero.
float materialPow5(float value) {
    value = max(value, 0.0);
    float squared = value * value;
    return squared * squared * value;
}

// Hm(theta) = (Cc_m*cos(m*theta) + Cs_m*sin(m*theta)) / sqrt(pi).
float3 circularHarmonicTerm(CH coefficients, uint order, float theta) {
    float cosine = cos(float(order) * theta);
    float sine = sin(float(order) * theta);
    return (coefficients.m_coefficients[2u * order - 1u] * cosine +
               coefficients.m_coefficients[2u * order] * sine) *
           kMaterialInvSqrtPi;
}

// pi * max(0, mean + length(N.xy) * sum(Hm(atan2(N.y, N.x)) / (1 + m*m))).
// Exactly zero XY returns pi*mean without evaluating atan2(0, 0).
float3 diffuseIrradiance(CH coefficients, float3 normal) {
    float3 mean = coefficients.m_coefficients[0] * kMaterialInvSqrtTwoPi;
    float length_xy = length(normal.xy);
    if (length_xy == 0.0) return kMaterialPi * mean;

    float theta = atan2(normal.y, normal.x);
    float3 sum = float3(0.0);
    for (uint order = 1u; order <= 4u; ++order) {
        sum += circularHarmonicTerm(coefficients, order, theta) /
               (1.0 + float(order * order));
    }

    return kMaterialPi * max(float3(0.0), mean + length_xy * sum);
}

// max(0, mean + sum(length(R.xy)^m * exp(-5*m*m*roughness*roughness)
//                  * Hm(atan2(R.y, R.x)))).
// The length(R.xy)^m factor makes every directional term vanish continuously at
// the pole instead of selecting an arbitrary azimuth. Exactly zero XY returns
// mean without evaluating atan2(0, 0).
float3 specularRadiance(CH coefficients, float3 reflection, float roughness) {
    float3 mean = coefficients.m_coefficients[0] * kMaterialInvSqrtTwoPi;
    float length_xy = length(reflection.xy);
    if (length_xy == 0.0) return mean;

    float theta = atan2(reflection.y, reflection.x);
    float squared_roughness = roughness * roughness;
    float3 sum = float3(0.0);
    for (uint order = 1u; order <= 4u; ++order) {
        float weight = pow(length_xy, float(order)) *
                       exp(-5.0 * float(order * order) * squared_roughness);
        sum += weight * circularHarmonicTerm(coefficients, order, theta);
    }

    return max(float3(0.0), mean + sum);
}

// Bilinearly samples the immutable 64x128 RG16F integration table at
// (NoV, roughness) with clamp-to-edge addressing. Both inputs are physical
// domains, so saturate before the normalized lookup.
float2 brdfLutPair(
    Texture2D<float2> lut, SamplerState sampler, float n_dot_v, float roughness) {
    return lut.SampleLevel(sampler, float2(saturate(n_dot_v), saturate(roughness)), 0.0);
}

// Full visible response. base_color is linear texture RGB times linear tint,
// saturated to [0,1]; authored and thermal emission are added once and are not
// modulated by reflectance. environment is the sampled (b, e) BRDF pair.
float3 visibleMaterialResponse(
    float3 base_color, float metallic, float roughness, float eta,
    float3 normal, float3 view, CH coefficients, float2 environment,
    float3 authored_emission, float3 thermal_emission) {
    float3 reflectance = saturate(base_color);
    float m = saturate(metallic);
    float r = saturate(roughness);
    float bounded_eta = max(eta, 1.0);

    float n_dot_v = max(dot(normal, view), 1.0e-5);
    float dielectric_f0 = (bounded_eta - 1.0) / (bounded_eta + 1.0);
    float3 f0 = lerp(float3(dielectric_f0 * dielectric_f0), reflectance, m);
    float3 fresnel = f0 + (max(float3(1.0 - r), f0) - f0) * materialPow5(1.0 - saturate(n_dot_v));
    float3 diffuse_weight = (1.0 - fresnel) * (1.0 - m) * reflectance / kMaterialPi;

    float3 reflection = reflect(-view, normal);
    float3 energy_compensation = 1.0 + f0 * (1.0 / max(environment.y, 1.0e-5) - 1.0);
    float3 specular_weight =
        max(float3(0.0), lerp(float3(environment.x), float3(environment.y), f0) * energy_compensation);

    return diffuse_weight * diffuseIrradiance(coefficients, normal) +
           specular_weight * specularRadiance(coefficients, reflection, r) +
           authored_emission + thermal_emission;
}

#endif
