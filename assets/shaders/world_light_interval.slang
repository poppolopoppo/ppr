// ------------------------------------------------------------------
// cascade world light field: level-interval trace (read-only source)
// ------------------------------------------------------------------
// The cell-by-cell DDA shared by the field's compute trace and the terrain
// fragment shading's in-place near trace. It reads the dense source through
// read-only StructuredBuffer declarations, so a module that only samples (the
// terrain shader, the evaluate seam) never has to bind the writing UAVs the
// source/matter passes use. The same buffers are bound read-only to the field
// update's roots.
#ifndef PPR_WORLD_LIGHT_INTERVAL_SLANG
#define PPR_WORLD_LIGHT_INTERVAL_SLANG

#include "world_light_field.slang"

// Dense source rows, read-only. The writer modules declare their own
// RWStructuredBuffer views; these names are distinct so both declarations can
// coexist in a module that includes the source ABI and this trace.
StructuredBuffer<uint> g_world_light_dense_ids;
StructuredBuffer<uint> g_world_light_dense_overlay;

// The per-cell radiance atlas sampler, defined by world_light_sample.slang.
// The atlas producer suppresses it (it owns the read/write atlas binding), and
// that producer never calls the trace, so the bounce compiles to zero there.
#ifndef PPR_WORLD_LIGHT_NO_ATLAS_SAMPLER
float3 worldLightAtlasRadiance(float2 world);
#endif

// Traces [nearM, farM) from origin along direction through the dense source
// with an Amanatides-Woo cell DDA, visiting every crossed cell including the
// offset start cell. The prototype radianceTraceMap/mapGradToRadiance path has
// no self-cell skip: a sample whose connector starts inside an emissive or
// opaque cell observes that cell. Opaque cells (material kind 1, or overlay
// coverage >= 0.5) stop the ray and emit; participating cells attenuate
// transmittance and emit volume radiance. Out-of-world segments contribute
// nothing.
[ForceInline]
void traceWorldLightInterval(
    float2 origin, float2 direction, float nearM, float farM, out float3 radiance, out float transmittance) {
    radiance = float3(0.0, 0.0, 0.0);
    transmittance = 1.0;

    float width = float(g_world_light_field.width);
    float height = float(g_world_light_field.height);
    float cellMetres = g_world_light_field.cellMetres;
    float originX = g_world_light_field.originX;
    float originY = g_world_light_field.originY;

    float2 base = origin + direction * nearM;
    float2 worldMin = float2(originX, originY);
    float2 worldMax = worldMin + float2(width, height) * cellMetres;

    // Clip the traced segment to the world bounds before walking cells. Outside
    // the world nothing emits or attenuates, so stepping those cells only wastes
    // work; the coarse levels start thousands of cells away and would otherwise
    // walk the whole interval through empty space on every sample. The slab test
    // keeps the terminal-sky transmittance semantics: out-of-world segments were
    // already neutral, so clipping them changes no radiance.
    float tEnter = 0.0;
    float tExit = farM - nearM;
    if (direction.x != 0.0) {
        float inv = 1.0 / direction.x;
        float a = (worldMin.x - base.x) * inv;
        float b = (worldMax.x - base.x) * inv;
        tEnter = max(tEnter, min(a, b));
        tExit = min(tExit, max(a, b));
    } else if (base.x < worldMin.x || base.x > worldMax.x) {
        return;
    }
    if (direction.y != 0.0) {
        float inv = 1.0 / direction.y;
        float a = (worldMin.y - base.y) * inv;
        float b = (worldMax.y - base.y) * inv;
        tEnter = max(tEnter, min(a, b));
        tExit = min(tExit, max(a, b));
    } else if (base.y < worldMin.y || base.y > worldMax.y) {
        return;
    }
    if (tExit <= tEnter) {
        return;
    }

    float2 start = base + direction * tEnter;
    float baseT = nearM + tEnter;
    float intervalLength = tExit - tEnter;

    float cellX = (start.x - originX) / cellMetres;
    float cellY = (start.y - originY) / cellMetres;
    int cx = int(floor(cellX));
    int cy = int(floor(cellY));

    int stepX = direction.x >= 0.0 ? 1 : -1;
    int stepY = direction.y >= 0.0 ? 1 : -1;

    float tMaxX = (direction.x == 0.0)
                      ? 1e30
                      : ((stepX > 0 ? float(cx + 1) : float(cx)) - cellX) * cellMetres / direction.x;
    float tMaxY = (direction.y == 0.0)
                      ? 1e30
                      : ((stepY > 0 ? float(cy + 1) : float(cy)) - cellY) * cellMetres / direction.y;
    float tDeltaX = (direction.x == 0.0) ? 1e30 : cellMetres / abs(direction.x);
    float tDeltaY = (direction.y == 0.0) ? 1e30 : cellMetres / abs(direction.y);

    float t = 0.0;

    // Every crossed cell participates, including the offset start cell. Do not
    // skip the child cell or any first-visited cell: the transport reference has
    // no self-occlusion exception.
    uint maxSteps = uint(width + height) + 4u;
    for (uint i = 0u; i < maxSteps && t < intervalLength && transmittance > 0.0; ++i) {
        float tNext = min(min(tMaxX, tMaxY), intervalLength);
        float seg = tNext - t;

        if (cx >= 0 && cy >= 0 && cx < int(width) && cy < int(height)) {
            uint cell = uint(cy) * uint(width) + uint(cx);
            uint id = g_world_light_dense_ids[cell];
            uint base = cell * kOverlayStride;

            float coverage = asfloat(g_world_light_dense_overlay[base + kOverlayCoverage]);
            float3 sigma = float3(0.0, 0.0, 0.0);
            float3 emission = float3(0.0, 0.0, 0.0);
            float3 albedo = float3(0.0, 0.0, 0.0);
            bool opaque = coverage >= 0.5;

            if (id != 0u) {
                WorldLightMaterial material = g_world_light_materials[id - 1u];
                sigma = float3(material.sigma[0], material.sigma[1], material.sigma[2]);
                emission = float3(material.emission[0], material.emission[1], material.emission[2]);
                albedo = float3(material.albedo[0], material.albedo[1], material.albedo[2]);
                opaque = opaque || material.kind == 1u;
            }

            sigma += float3(
                asfloat(g_world_light_dense_overlay[base + kOverlaySigma0]),
                asfloat(g_world_light_dense_overlay[base + kOverlaySigma1]),
                asfloat(g_world_light_dense_overlay[base + kOverlaySigma2]));
            emission += float3(
                asfloat(g_world_light_dense_overlay[base + kOverlayEmission0]),
                asfloat(g_world_light_dense_overlay[base + kOverlayEmission1]),
                asfloat(g_world_light_dense_overlay[base + kOverlayEmission2]));

            float2 p = origin + direction * (baseT + t + 0.5 * seg);

            if (opaque) {
#ifndef PPR_WORLD_LIGHT_NO_ATLAS_SAMPLER
                float3 feedback = albedo * worldLightAtlasRadiance(p);
#else
                float3 feedback = float3(0.0, 0.0, 0.0);
#endif
                radiance += transmittance * (emission + feedback);
                transmittance = 0.0;
                break;
            }

            float extinction = dot(sigma, float3(1.0, 1.0, 1.0)) / 3.0;
            if (extinction > 0.0) {
                float transmit = exp(-extinction * seg);
                radiance += transmittance * emission * ((1.0 - transmit) / max(extinction, 1e-5));
                transmittance *= transmit;
            }
        }

        if (tMaxX < tMaxY) {
            t = tMaxX;
            tMaxX += tDeltaX;
            cx += stepX;
        } else {
            t = tMaxY;
            tMaxY += tDeltaY;
            cy += stepY;
        }
    }
}

#endif
