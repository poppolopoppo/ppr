// ------------------------------------------------------------------
// per-cell radiance atlas: packed RGB9E5 emit + one-bounce temporal EMA
// ------------------------------------------------------------------
// One packed u32 per native cell. The value is RGB9E5 (9-bit mantissas, shared
// 5-bit exponent) so a whole cell costs four bytes. Each dispatch rewrites
// every cell in place: new = material emission + overlay emission + albedo *
// previous atlas value (one bounce), then mix(new, previous,
// exp(-temporalRate * frameDelta)). One thread owns one cell, so reading the
// previous value and writing the new one needs no ping-pong buffer.
#ifndef PPR_WORLD_LIGHT_RADIANCE_ATLAS_SLANG
#define PPR_WORLD_LIGHT_RADIANCE_ATLAS_SLANG

#include "world_light_source.slang"

struct WorldLightRadianceAtlasConstants {
    uint width, height;
    float temporalRate;
    float frameDelta;
};

ConstantBuffer<WorldLightRadianceAtlasConstants> g_world_light_atlas_constants;
RWStructuredBuffer<uint> g_world_light_atlas;
StructuredBuffer<WorldLightMaterial> g_world_light_materials;

// ------------------------------------------------------------------
// per-cell update
// ------------------------------------------------------------------

// One thread per native cell. Material and overlay emission are the direct
// term; the material albedo multiplies the previous atlas value for a single
// bounce. Vacuum cells (id 0 with no overlay coverage or emission) are written
// as exactly zero and keep no history, so air never accumulates radiance and a
// cell that becomes vacuum clears in one pass. An extinction-only overlay
// (coverage > 0, emission 0) is deliberately not vacuum: that matches the
// source's occupancy oracle, so it decays history instead of clearing at once.
[shader("compute")]
[numthreads(64, 1, 1)]
void updateWorldLightRadianceAtlas(uint3 id : SV_DispatchThreadID) {
    uint width = g_world_light_atlas_constants.width;
    uint height = g_world_light_atlas_constants.height;
    uint cell = id.x;
    if (cell >= width * height) return;

    uint cellId = g_world_light_ids[cell];
    uint base = cell * kOverlayStride;

    float3 emission = float3(0.0, 0.0, 0.0);
    float3 albedo = float3(0.0, 0.0, 0.0);
    if (cellId != 0u) {
        WorldLightMaterial material = g_world_light_materials[cellId - 1u];
        emission = float3(material.emission[0], material.emission[1], material.emission[2]);
        albedo = float3(material.albedo[0], material.albedo[1], material.albedo[2]);
    }

    float coverage = asfloat(g_world_light_overlay[base + kOverlayCoverage]);
    emission += float3(
        asfloat(g_world_light_overlay[base + kOverlayEmission0]),
        asfloat(g_world_light_overlay[base + kOverlayEmission1]),
        asfloat(g_world_light_overlay[base + kOverlayEmission2]));

    if (cellId == 0u && coverage <= 0.0 && emission.x == 0.0 && emission.y == 0.0 && emission.z == 0.0) {
        g_world_light_atlas[cell] = 0u;
        return;
    }

    float3 previous = unpackRadianceRGB9E5(g_world_light_atlas[cell]);
    float3 sample = emission + albedo * previous;
    float previousWeight =
        exp(-g_world_light_atlas_constants.temporalRate * g_world_light_atlas_constants.frameDelta);
    g_world_light_atlas[cell] = packRadianceRGB9E5(lerp(sample, previous, previousWeight));
}

#endif
