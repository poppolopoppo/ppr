// ------------------------------------------------------------------
// per-cell incident radiance atlas: packed RGB9E5 + one-level gather + EMA
// ------------------------------------------------------------------
// One packed u32 per native cell. The value is RGB9E5 (9-bit mantissas, shared
// 5-bit exponent) so a whole cell costs four bytes. The atlas stores the
// incident radiance L_i arriving at the cell centre from the previous frame's
// fully merged field -- never the cell's own emission or albedo. Consumers add
// their own emission exactly once and multiply L_i by their albedo (the trace's
// opaque-hit bounce and the terrain ambient term), so the producer adds no
// emission or albedo term and the value never feeds back directly; a
// non-emissive cell still carries the light around it. Indirect feedback through
// the field is bounded by kAtlasRadianceCeiling below.
//
// Each dispatch rewrites every cell in place: new = mix(L_i, previous,
// exp(-temporalRate * frameDelta)), the temporal EMA that keeps a settled value
// stable without a ping-pong buffer (one thread owns one cell).
#ifndef PPR_WORLD_LIGHT_RADIANCE_ATLAS_SLANG
#define PPR_WORLD_LIGHT_RADIANCE_ATLAS_SLANG

// Documented bound on the stored atlas value, applied after the EMA. The atlas
// feeds the field and the field feeds the atlas: the closed-loop pole is
// w + (1-w)*G with G = albedo_avg * transmittance_avg * gather_avg <= 1. At
// G = 1 that pole is exactly 1 regardless of the retention weight w, so the
// fixed point is P/(1-G) and retention only slows the transient. That cannot
// produce NaN/inf/negative (every step is a convex combination of finite
// values), but it can saturate the RGB9E5 range and blow out. This is a bound,
// not a tuning knob: the validated peak is 2.5, so normal operation never
// reaches it.
static const float kAtlasRadianceCeiling = 16.0;

// The producer owns the read/write atlas binding below; suppress the shared
// sampler's read-only declaration. The sample module also provides
// worldLightDiffuseFrom, the single-level gather both consumers use.
#define PPR_WORLD_LIGHT_NO_ATLAS_SAMPLER
#include "world_light_sample.slang"

struct WorldLightRadianceAtlasConstants {
    uint width, height;
    float temporalRate;
    float frameDelta;
};

ConstantBuffer<WorldLightRadianceAtlasConstants> g_world_light_atlas_constants;
RWStructuredBuffer<uint> g_world_light_atlas;

// ------------------------------------------------------------------
// per-cell update
// ------------------------------------------------------------------

// One thread per native cell. Incident radiance is the +Z diffuse response of
// the merged field at the cell centre: level 0 already holds every level's
// interval, so this is one gather per cell instead of one per ray hit. Vacuum
// cells (id 0 with no overlay coverage or emission) are written as exactly zero
// and keep no history, so air never accumulates radiance and a cell that
// becomes vacuum clears in one pass. An extinction-only overlay (coverage > 0,
// emission 0) is deliberately not vacuum: that matches the source's occupancy
// oracle, so it decays history instead of clearing at once.
[shader("compute")]
[numthreads(64, 1, 1)]
void updateWorldLightRadianceAtlas(uint3 id : SV_DispatchThreadID) {
    uint width = g_world_light_atlas_constants.width;
    uint height = g_world_light_atlas_constants.height;
    uint cell = id.x;
    if (cell >= width * height) return;

    uint cellId = g_world_light_ids[cell];
    uint base = cell * kOverlayStride;

    float coverage = asfloat(g_world_light_overlay[base + kOverlayCoverage]);
    float3 overlayEmission = float3(
        asfloat(g_world_light_overlay[base + kOverlayEmission0]),
        asfloat(g_world_light_overlay[base + kOverlayEmission1]),
        asfloat(g_world_light_overlay[base + kOverlayEmission2]));

    if (cellId == 0u && coverage <= 0.0 && overlayEmission.x == 0.0 && overlayEmission.y == 0.0 &&
        overlayEmission.z == 0.0) {
        g_world_light_atlas[cell] = 0u;
        return;
    }

    uint px = cell % width;
    uint py = cell / width;
    float2 origin = float2(g_world_light_field.originX, g_world_light_field.originY);
    float2 centre = origin + (float2(float(px), float(py)) + 0.5) * g_world_light_field.cellMetres;

    float3 incident = worldLightDiffuseFrom(g_world_light_radiance_prev, centre, float3(0.0, 0.0, 1.0));

    float3 previous = unpackRadianceRGB9E5(g_world_light_atlas[cell]);
    float previousWeight =
        exp(-g_world_light_atlas_constants.temporalRate * g_world_light_atlas_constants.frameDelta);
    float3 stored = min(lerp(incident, previous, previousWeight), kAtlasRadianceCeiling);
    g_world_light_atlas[cell] = packRadianceRGB9E5(stored);
}

#endif
