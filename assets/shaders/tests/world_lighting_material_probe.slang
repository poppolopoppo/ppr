// Test-only probe for the shared visible material response, CH filters, BRDF
// table sampling, fine-CH angular mean and the categorical texture/normal
// helpers. It includes the real grid_tiles program so the production helpers
// are exercised verbatim; no runtime entry point consumes this file.
#include "../grid_tiles.slang"

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
RWStructuredBuffer<float4> g_probe_detail;

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

[shader("compute")]
[numthreads(64, 1, 1)]
void probeView(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_material_probe.m_count) return;

    // field 0: camera origin xyz, cell size w
    // field 1: camera forward xyz, orthographic flag w
    // field 2: fine position xy
    // field 3: world normal xyz
    float4 origin = probeField(thread.x, 0u);
    float4 forward = probeField(thread.x, 1u);
    float4 fine = probeField(thread.x, 2u);
    float4 normal = probeField(thread.x, 3u);
    float3 view = planeSurfaceToCamera(
        fine.xy, origin.w, origin.xyz, forward.xyz, forward.w != 0.0);
    float n_dot_v = visibleNoV(normalize(normal.xyz), view);
    g_probe_detail[thread.x * 2u + 0u] = float4(view, n_dot_v);
    g_probe_detail[thread.x * 2u + 1u] =
        float4(visibleFresnel(float3(0.04), 0.5, n_dot_v), 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probeCategoricalBase(uint3 thread : SV_DispatchThreadID,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) {
    if (thread.x >= g_material_probe.m_count) return;

    // field 0: gradient x xy, gradient y xy
    // field 1: texture period, cell size
    // field 2: fine position xy
    // field 3: material id in x
    uint material = uint(probeField(thread.x, 3u).x);
    GridVisual visual = g_terrain_visuals[material];
    float4 gradients = probeField(thread.x, 0u);
    float4 parameters = probeField(thread.x, 1u);
    float2 fine = probeField(thread.x, 2u).xy;
    float2 gradient_x = gradients.xy;
    float2 gradient_y = gradients.zw;
    float period = parameters.x;
    float2 world = fine * parameters.y;
    float2 uv = world / period;
    bool repeat_texture = (visual.m_surface_flags & kSurfaceTextureRepeat) != 0u;
    TexturePatches patches;
    if (repeat_texture) {
        patches.uv0 = frac(uv);
        patches.uv1 = patches.uv0;
        patches.uv2 = patches.uv0;
        patches.weights = float3(1.0, 0.0, 0.0);
    } else {
        patches = texturePatches(uv);
    }

    float2 uv_gradient_x = textureUvGradient(gradient_x, period);
    float2 uv_gradient_y = textureUvGradient(gradient_y, period);
    float4 raw = sampleVisibleAlbedo(
        visual, patches, repeat_texture, uv_gradient_x, uv_gradient_y, g_textures, g_sampler);
    float4 base = normalizeVisibleAlbedo(visual, raw, patches.weights, repeat_texture);
    float3 normal = sampleVisibleNormal(
        visual, patches, repeat_texture, uv_gradient_x, uv_gradient_y, g_textures, g_sampler);

    uint base_index = thread.x * 8u;
    g_probe_detail[base_index + 0u] = float4(base.rgb, base.a);
    g_probe_detail[base_index + 1u] = raw;
    g_probe_detail[base_index + 2u] = float4(patches.weights, 1.0);
    g_probe_detail[base_index + 3u] = float4(patches.uv0, patches.uv1);
    g_probe_detail[base_index + 4u] = float4(patches.uv2, normal.xy);
    g_probe_detail[base_index + 5u] = float4(normal.z, uv_gradient_x.x, uv_gradient_x.y, 0.0);
    g_probe_detail[base_index + 6u] = float4(uv_gradient_y.x, uv_gradient_y.y, 0.0, 0.0);
    g_probe_detail[base_index + 7u] = float4(
        visual.m_filtered_mean_r, visual.m_filtered_mean_g, visual.m_filtered_mean_b, 0.0);
}

// ------------------------------------------------------------------
// particle terrain-occlusion decision
// ------------------------------------------------------------------
// Reads the committed resolved/ownership words and the real palette record for
// each probed native cell and reports the shared occlusion decision. The probe
// exercises grid_tiles' production helper; the particle fragment applies it.
[shader("compute")]
[numthreads(64, 1, 1)]
void probeTerrainOcclusion(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_material_probe.m_count) return;

    // field 0: committed native-cell index in x
    uint cell = uint(probeField(thread.x, 0u).x);
    uint resolved = g_resolved_cells[cell];
    uint resolved_material = unpackCellMaterial(resolved);
    GridVisual terrain = g_terrain_visuals[min(resolved_material, 65535u)];
    uint best_row = g_best_row[cell];
    bool occludes = terrainOccludesParticle(resolved_material, terrain.m_material_id,
        (terrain.m_surface_flags & kSurfaceOpticalBlend) != 0u, best_row);
    // Exactly representable diagnostics: decision, resolved material id and the
    // committed-ownership flag. No raw 32-bit word is cast through float.
    g_probe_detail[thread.x] = float4(occludes ? 1.0 : 0.0, float(resolved_material),
        best_row != 0xffffffffu ? 1.0 : 0.0, 0.0);
}
