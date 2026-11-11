// Test-only probe for the shared visible material response, CH filters, BRDF
// table sampling and fine-CH angular mean. The production helpers live in
// ../world_lighting_material.slang and ../world_lighting_ch.slang; no runtime
// entry point consumes this file.
#include "../world_lighting_codec.slang"

StructuredBuffer<PackedCH> g_fine_ch;
#include "../world_lighting_ch.slang"
#include "../world_lighting_material.slang"

struct MaterialProbeFrame {
    uint m_count;
    uint m_reserved[3];
};

ConstantBuffer<MaterialProbeFrame> g_material_probe;
// Eight float4 records per case: base, parameters, normal/reflection, view,
// environment pair, authored emission, thermal emission, lookup position.
StructuredBuffer<float4> g_probe_fields;
StructuredBuffer<PackedCH> g_probe_ch;
Texture2D<float2> g_probe_brdf;
SamplerState g_probe_brdf_sampler;
RWStructuredBuffer<float4> g_probe_output;

float4 probeField(uint index, uint field) {
    return g_probe_fields[index * 8u + field];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probeResponse(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_material_probe.m_count) return;

    float4 base = probeField(thread.x, 0u);
    float4 parameters = probeField(thread.x, 1u);
    float4 normal = probeField(thread.x, 2u);
    float4 view = probeField(thread.x, 3u);
    float4 environment = probeField(thread.x, 4u);
    float4 authored = probeField(thread.x, 5u);
    float4 thermal = probeField(thread.x, 6u);
    CH coefficients = unpackCH(g_probe_ch[thread.x]);
    float3 hdr = visibleMaterialResponse(
        base.rgb, parameters.x, parameters.y, parameters.z, normalize(normal.xyz), normalize(view.xyz),
        coefficients, environment.xy, authored.rgb, thermal.rgb);
    g_probe_output[thread.x] = float4(hdr, 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probeDiffuse(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_material_probe.m_count) return;
    CH coefficients = unpackCH(g_probe_ch[thread.x]);
    float3 irradiance = diffuseIrradiance(coefficients, normalize(probeField(thread.x, 2u).xyz));
    g_probe_output[thread.x] = float4(irradiance, 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probeSpecular(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_material_probe.m_count) return;
    CH coefficients = unpackCH(g_probe_ch[thread.x]);
    float3 radiance =
        specularRadiance(coefficients, normalize(probeField(thread.x, 2u).xyz), probeField(thread.x, 1u).y);
    g_probe_output[thread.x] = float4(radiance, 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probeBrdf(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_material_probe.m_count) return;
    float4 position = probeField(thread.x, 7u);
    float2 pair = brdfLutPair(g_probe_brdf, g_probe_brdf_sampler, position.x, position.y);
    g_probe_output[thread.x] = float4(pair, 0.0, 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probeAngularMean(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_material_probe.m_count) return;
    float4 position = probeField(thread.x, 7u);
    g_probe_output[thread.x] = float4(angularMean(position.xy), 0.0);
}
