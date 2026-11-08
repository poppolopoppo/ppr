// ------------------------------------------------------------------
// cascade world light field: level-interval trace and temporal update
// ------------------------------------------------------------------
// One thread per (probe, direction): traces the level's own interval through
// the dense source (cell ids + material palette + overlay + occupancy),
// accumulates the sample into that direction's running mean, and writes the
// current radiance buffer. Task 7 supplies the previous-hierarchy composite;
// for Task 6 the hit-shading feedback hook returns zero (direct emission only).
#include "world_light_field.slang"

static const float kTau = 6.28318530718;

// Fraction of the angular bin / probe footprint each sample is jittered by
// (temporal supersampling). Kept deliberately small for Task 6 so a single-cell
// emitter is reliably hit; the range is a tunable quality parameter.
static const float kSampleJitter = 0.2;

// ------------------------------------------------------------------
// deterministic low-discrepancy jitter
// ------------------------------------------------------------------

[ForceInline]
uint hash32(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x = x + (x << 3u);
    x = x ^ (x >> 4u);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return x;
}

// Deterministic [0,1) from two integer inputs (probe index, frame).
[ForceInline]
float hash01(uint a, uint b) {
    return float(hash32(a ^ (b * 0x9e3779b1u))) / float(0xffffffffu);
}

// ------------------------------------------------------------------
// occupancy and feedback
// ------------------------------------------------------------------

// True when the 8x8 block containing the cell has its occupancy bit set.
[ForceInline]
bool blockOccupied(int2 cell) {
    uint width = g_world_light_field.width;
    uint blocksX = (width + 7u) / 8u;
    uint wordsPerRow = (blocksX + 31u) / 32u;
    uint bx = uint(cell.x) / 8u;
    uint by = uint(cell.y) / 8u;
    uint word = by * wordsPerRow + bx / 32u;
    uint bit = bx % 32u;
    return (g_world_light_occupancy[word] & (1u << bit)) != 0u;
}

// Zero-feedback hook: Task 7 reads the previous hierarchy composite here so a
// hit adds the material response of the previously completed field. For Task 6
// the field accumulates primary transport only.
[ForceInline]
float3 worldLightFeedback(float2 position, float2 direction) {
    return float3(0.0, 0.0, 0.0);
}

// ------------------------------------------------------------------
// level-interval trace
// ------------------------------------------------------------------

// Traces [nearM, farM) from origin along direction through the dense source.
// Opaque cells (material kind 1, or overlay coverage >= 0.5) stop the ray and
// emit; participating cells attenuate transmittance and emit volume radiance.
// Empty 8x8 blocks are skipped with a coarse step; occupied blocks march finely.
[ForceInline]
void traceWorldLightInterval(
    float2 origin, float2 direction, float nearM, float farM, out float3 radiance, out float transmittance) {
    radiance = float3(0.0, 0.0, 0.0);
    transmittance = 1.0;

    float width = float(g_world_light_field.width);
    float height = float(g_world_light_field.height);
    float cellMetres = g_world_light_field.cellMetres;

    const float fineStep = max(0.25 * cellMetres, 1e-4);
    const float coarseStep = 8.0 * cellMetres;

    float t = nearM;
    uint maxSteps = uint((farM - nearM) / fineStep) + 8u;
    for (uint i = 0u; i < maxSteps && t < farM && transmittance > 0.0; ++i) {
        float2 p = origin + direction * t;
        float cx = (p.x - g_world_light_field.originX) / cellMetres;
        float cy = (p.y - g_world_light_field.originY) / cellMetres;
        if (cx < 0.0 || cy < 0.0 || cx >= width || cy >= height) break;

        int2 cell = int2(int(cx), int(cy));
        float step = blockOccupied(cell) ? fineStep : coarseStep;

        uint id = g_world_light_ids[cell.y * int(width) + cell.x];
        uint base = uint(cell.y * int(width) + cell.x) * kOverlayStride;

        float coverage = asfloat(g_world_light_overlay[base + kOverlayCoverage]);
        float3 sigma = float3(0.0, 0.0, 0.0);
        float3 emission = float3(0.0, 0.0, 0.0);
        bool opaque = coverage >= 0.5;

        if (id != 0u) {
            WorldLightMaterial material = g_world_light_materials[id - 1u];
            sigma = float3(material.sigma[0], material.sigma[1], material.sigma[2]);
            emission = float3(material.emission[0], material.emission[1], material.emission[2]);
            opaque = opaque || material.kind == 1u;
        }

        sigma += float3(
            asfloat(g_world_light_overlay[base + kOverlaySigma0]),
            asfloat(g_world_light_overlay[base + kOverlaySigma1]),
            asfloat(g_world_light_overlay[base + kOverlaySigma2]));
        emission += float3(
            asfloat(g_world_light_overlay[base + kOverlayEmission0]),
            asfloat(g_world_light_overlay[base + kOverlayEmission1]),
            asfloat(g_world_light_overlay[base + kOverlayEmission2]));

        if (opaque) {
            radiance += transmittance * (emission + worldLightFeedback(p, direction));
            transmittance = 0.0;
            break;
        }

        float extinction = dot(sigma, float3(1.0, 1.0, 1.0)) / 3.0;
        if (extinction > 0.0) {
            float transmit = exp(-extinction * step);
            radiance += transmittance * emission * ((1.0 - transmit) / max(extinction, 1e-5));
            transmittance *= transmit;
        }

        t += step;
    }
}

// ------------------------------------------------------------------
// temporal update entry
// ------------------------------------------------------------------

[numthreads(64, 1, 1)]
void updateWorldLightField(uint3 id : SV_DispatchThreadID) {
    uint level = g_world_light_dispatch.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint local = g_world_light_dispatch.sampleBegin + id.x;
    if (local >= L.probesX * L.probesY * L.directions) return;

    uint probe = local / L.directions;
    uint direction = local % L.directions;
    uint sample = L.sampleOffset + local;

    uint px = probe % L.probesX;
    uint py = probe / L.probesX;
    uint probeLinear = L.probeOffset + probe;
    uint frame = g_world_light_field.frame;

    // Direction jitter within the angular bin, and probe-origin jitter within
    // the footprint, both deterministic from (probe, frame).
    float jitterAngle = (hash01(probeLinear, frame) - 0.5) * kSampleJitter;
    float2 jitterPos = (float2(hash01(probeLinear + 1u, frame), hash01(probeLinear + 2u, frame)) - 0.5) * kSampleJitter;

    float angle = (float(direction) + 0.5 + jitterAngle) * kTau / float(L.directions);
    float2 dir = float2(cos(angle), sin(angle));

    float spacing = float(L.spacing);
    float2 cellCenter = float2(float(px) + 0.5, float(py) + 0.5) * spacing + jitterPos * spacing;
    float2 origin = float2(g_world_light_field.originX, g_world_light_field.originY) +
                    cellCenter * g_world_light_field.cellMetres;

    float3 radiance;
    float transmittance;
    traceWorldLightInterval(origin, dir, L.nearM, L.farM, radiance, transmittance);

    WorldLightRadiance previous = g_world_light_radiance_prev[sample];
    float3 previousRadiance = float3(previous.radianceR, previous.radianceG, previous.radianceB);
    float previousTransmittance = float(previous.transmittance);

    uint count = g_world_light_state[sample];
    uint newCount = min(count + 1u, 255u);
    float blend = 1.0 / float(newCount);

    float3 newRadiance = previousRadiance + (radiance - previousRadiance) * blend;
    float newTransmittance = previousTransmittance + (transmittance - previousTransmittance) * blend;

    WorldLightRadiance result;
    result.radianceR = (half)newRadiance.x;
    result.radianceG = (half)newRadiance.y;
    result.radianceB = (half)newRadiance.z;
    result.transmittance = (half)newTransmittance;

    g_world_light_radiance_cur[sample] = result;
    g_world_light_state[sample] = newCount;
}

// ------------------------------------------------------------------
// dirty reset entry
// ------------------------------------------------------------------

[numthreads(64, 1, 1)]
void resetWorldLightField(uint3 id : SV_DispatchThreadID) {
    uint level = g_world_light_reset.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];

    uint local = id.x;
    uint rectWidth = g_world_light_reset.rectWidth;
    uint px = g_world_light_reset.probeX0 + (local % rectWidth);
    uint py = g_world_light_reset.probeY0 + (local / rectWidth);
    if (px >= L.probesX || py >= L.probesY) return;

    uint probe = py * L.probesX + px;
    uint sample = L.sampleOffset + probe * L.directions;
    for (uint d = 0u; d < L.directions; ++d) {
        g_world_light_state[sample + d] = 0u;
        g_world_light_radiance_prev[sample + d] = (WorldLightRadiance)0;
        g_world_light_radiance_cur[sample + d] = (WorldLightRadiance)0;
    }
}
