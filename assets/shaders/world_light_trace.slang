// ------------------------------------------------------------------
// cascade world light field: level-interval trace and temporal update
// ------------------------------------------------------------------
// One thread per (probe, direction) for the cursor, and one thread per probe
// for the dirty-first reprocess. Each traces the level's own interval through
// the dense source (cell ids + material palette + overlay) with a cell-by-cell
// DDA, accumulates the sample into the running mean, and writes the current
// radiance buffer. An opaque hit adds one bounce of the previously completed
// field weighted by the hit material's albedo, so the field accumulates primary
// transport plus a single material-weighted bounce (no recursive ray tree).
#include "world_light_field.slang"
#include "world_light_sample.slang"

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
// feedback
// ------------------------------------------------------------------

// One previous completed-field bounce at an opaque hit, added inline in the
// trace: feedback = material.albedo * worldLightDiffusePrev(hitPosition, normal).
// The face normal comes from the DDA step that entered the hit cell (see the
// trace loop), so the diffuse term samples the hemisphere the ray crossed. An
// overlay-only opaque surface (no material) has zero albedo and contributes no
// bounce. There is no recursive ray tree or face bake: exactly one previous
// field read per opaque hit.

// ------------------------------------------------------------------
// level-interval trace (cell-by-cell DDA)
// ------------------------------------------------------------------

// Traces [nearM, farM) from origin along direction through the dense source
// with an Amanatides-Woo cell DDA, visiting every cell the ray crosses exactly
// once. Opaque cells (material kind 1, or overlay coverage >= 0.5) stop the ray
// and emit; participating cells attenuate transmittance and emit volume
// radiance. Out-of-world segments contribute nothing.
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

    float2 start = origin + direction * nearM;
    float intervalLength = farM - nearM;

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
    float3 entryNormal = float3(0.0, 0.0, 1.0); // +Z for the ray-start cell.
    uint maxSteps = uint(width + height) + 4u;
    for (uint i = 0u; i < maxSteps && t < intervalLength && transmittance > 0.0; ++i) {
        float tNext = min(min(tMaxX, tMaxY), intervalLength);
        float seg = tNext - t;

        if (cx >= 0 && cy >= 0 && cx < int(width) && cy < int(height)) {
            uint cell = uint(cy) * uint(width) + uint(cx);
            uint id = g_world_light_ids[cell];
            uint base = cell * kOverlayStride;

            float coverage = asfloat(g_world_light_overlay[base + kOverlayCoverage]);
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
                asfloat(g_world_light_overlay[base + kOverlaySigma0]),
                asfloat(g_world_light_overlay[base + kOverlaySigma1]),
                asfloat(g_world_light_overlay[base + kOverlaySigma2]));
            emission += float3(
                asfloat(g_world_light_overlay[base + kOverlayEmission0]),
                asfloat(g_world_light_overlay[base + kOverlayEmission1]),
                asfloat(g_world_light_overlay[base + kOverlayEmission2]));

            float2 p = origin + direction * (nearM + t + 0.5 * seg);

            if (opaque) {
                float3 feedback = albedo * worldLightDiffusePrev(p, entryNormal);
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
            entryNormal = float3(-float(stepX), 0.0, 0.0);
        } else {
            t = tMaxY;
            tMaxY += tDeltaY;
            cy += stepY;
            entryNormal = float3(0.0, -float(stepY), 0.0);
        }
    }
}

// ------------------------------------------------------------------
// accumulation
// ------------------------------------------------------------------

// Traces and accumulates one direction sample of one probe into the running
// mean, writing the current radiance buffer and the sample count.
[ForceInline]
void accumulateSample(uint level, uint probe, uint direction) {
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint sample = L.sampleOffset + probe * L.directions + direction;

    uint px = probe % L.probesX;
    uint py = probe / L.probesX;
    uint probeLinear = L.probeOffset + probe;
    uint frame = g_world_light_field.frame;

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
    uint newCount = count < g_world_light_field.accumulationSamples ? count + 1u
                                                                    : g_world_light_field.accumulationSamples;

    // Running mean while accumulating toward the cap; a per-60Hz retention EMA
    // afterward, scaled by the injected frame delta so a longer frame decays the
    // history proportionally more. Both weights stay in [0,1], so the result is
    // a convex combination of finite samples and never leaves the [0,..] range.
    float blend;
    if (count < g_world_light_field.accumulationSamples) {
        blend = 1.0 / float(newCount);
    } else {
        float perFrameRetention = pow(g_world_light_field.historyRetention, g_world_light_field.frameDelta * 60.0);
        blend = 1.0 - perFrameRetention;
    }

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
// cursor update entry
// ------------------------------------------------------------------

[numthreads(64, 1, 1)]
void updateWorldLightField(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_world_light_dispatch.sampleCount) return;

    uint level = g_world_light_dispatch.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint local = g_world_light_dispatch.sampleBegin + id.x;
    uint probe = local / L.directions;
    uint direction = local % L.directions;

    accumulateSample(level, probe, direction);
}

// ------------------------------------------------------------------
// dirty-first update entry
// ------------------------------------------------------------------

[numthreads(64, 1, 1)]
void updateWorldLightFieldProbe(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_world_light_probe_update.probeCount) return;

    uint level = g_world_light_probe_update.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint rectWidth = g_world_light_probe_update.rectWidth;
    uint px = g_world_light_probe_update.probeX0 + (id.x % rectWidth);
    uint py = g_world_light_probe_update.probeY0 + (id.x / rectWidth);
    if (px >= L.probesX || py >= L.probesY) return;

    uint probe = py * L.probesX + px;
    for (uint d = 0u; d < L.directions; ++d) {
        accumulateSample(level, probe, d);
    }
}

// ------------------------------------------------------------------
// composite evaluate (test seam)
// ------------------------------------------------------------------

// One-shot sampler evaluation for a supplied position/direction/normal. The mode
// selects composite (0), diffuse (1) or specular (2); the RGB result is written
// to g_world_light_evaluate_out. Test-only readback path.
struct WorldLightEvaluateParams {
    float positionX, positionY;
    float directionX, directionY;
    float normalX, normalY, normalZ;
    float roughness;
    uint mode;
    uint pad0, pad1, pad2;
};

ConstantBuffer<WorldLightEvaluateParams> g_world_light_evaluate;
RWStructuredBuffer<float> g_world_light_evaluate_out;

[numthreads(1, 1, 1)]
void evaluateWorldLightField(uint3 id : SV_DispatchThreadID) {
    float2 position = float2(g_world_light_evaluate.positionX, g_world_light_evaluate.positionY);
    float2 direction = float2(g_world_light_evaluate.directionX, g_world_light_evaluate.directionY);
    float3 normal = float3(g_world_light_evaluate.normalX, g_world_light_evaluate.normalY, g_world_light_evaluate.normalZ);

    float3 result;
    if (g_world_light_evaluate.mode == 0u) {
        result = worldLightComposite(position, direction);
    } else if (g_world_light_evaluate.mode == 1u) {
        result = worldLightDiffuse(position, normal);
    } else {
        result = worldLightSpecular(position, float3(direction, 0.0), g_world_light_evaluate.roughness);
    }
    g_world_light_evaluate_out[0] = result.x;
    g_world_light_evaluate_out[1] = result.y;
    g_world_light_evaluate_out[2] = result.z;
}

// ------------------------------------------------------------------
// dirty reset entry
// ------------------------------------------------------------------

[numthreads(64, 1, 1)]
void resetWorldLightField(uint3 id : SV_DispatchThreadID) {
    uint rectWidth = g_world_light_reset.rectWidth;
    uint rectHeight = g_world_light_reset.rectHeight;
    if (id.x >= rectWidth * rectHeight) return;

    uint level = g_world_light_reset.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint px = g_world_light_reset.probeX0 + (id.x % rectWidth);
    uint py = g_world_light_reset.probeY0 + (id.x / rectWidth);
    if (px >= L.probesX || py >= L.probesY) return;

    uint probe = py * L.probesX + px;
    uint sample = L.sampleOffset + probe * L.directions;
    for (uint d = 0u; d < L.directions; ++d) {
        g_world_light_state[sample + d] = 0u;
        g_world_light_radiance_prev[sample + d] = (WorldLightRadiance)0;
        g_world_light_radiance_cur[sample + d] = (WorldLightRadiance)0;
    }
}
