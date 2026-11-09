// ------------------------------------------------------------------
// cascade world light field: level-interval trace and temporal update
// ------------------------------------------------------------------
// One thread per (probe, direction) for the cursor, and one thread per probe
// for the dirty-first reprocess. Each traces the level's own interval through
// the dense source (cell ids + material palette + overlay) with a cell-by-cell
// DDA, folds in its parent level's already-merged suffix (L = L_near + T_near *
// L_far), accumulates the sample into the running mean, and writes the current
// radiance buffer. Level 0 therefore holds the fully merged field. The coarsest
// level has no parent: it merges each bin with the reference angular sky
// integral over the four next-level directions it covers. An opaque hit adds one
// bounce of the per-cell radiance atlas weighted by the hit material's albedo, so
// the field accumulates primary transport plus a single material-weighted bounce
// (no recursive ray tree).
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

// One atlas sample at an opaque hit, added inline in the trace:
// feedback = material.albedo * worldLightAtlasRadiance(hitPosition). The atlas
// holds the packed per-cell radiance for this frame, so the bounce costs one
// load and no longer gathers the cascade directions. An overlay-only opaque
// surface (no material) has zero albedo and contributes no bounce. There is no
// recursive ray tree or face bake: exactly one atlas read per opaque hit.

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

    // A probe records the radiance ARRIVING at it, never the cell it sits in.
    // Treating the probe's own cell as an opaque hit made every solid probe
    // self-occlude, zero its transmittance and therefore zero the whole cascade
    // composite. Intervals no longer start at the probe centre (level 0 begins
    // at c0), so the rule is stated on the cell index rather than the distance:
    // if the FIRST visited cell is the cell containing the origin, skip it. A
    // surface probe then sees the light crossing into it from the neighbouring
    // air, which is what lights the terrain and bleeds around dug borders.
    const int originCx = int(floor((origin.x - originX) / cellMetres));
    const int originCy = int(floor((origin.y - originY) / cellMetres));
    const bool skipOriginCell = cx == originCx && cy == originCy;
    uint maxSteps = uint(width + height) + 4u;
    for (uint i = 0u; i < maxSteps && t < intervalLength && transmittance > 0.0; ++i) {
        float tNext = min(min(tMaxX, tMaxY), intervalLength);
        float seg = tNext - t;

        if ((i != 0u || !skipOriginCell) && cx >= 0 && cy >= 0 && cx < int(width) && cy < int(height)) {
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

            float2 p = origin + direction * (baseT + t + 0.5 * seg);

            if (opaque) {
                float3 feedback = albedo * worldLightAtlasRadiance(p);
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

// ------------------------------------------------------------------
// reference angular sky integral
// ------------------------------------------------------------------

// Analytic sky radiance over an angular interval, from "Analytic Direct
// Illumination" (Mathis, shadertoy NttSW7), exactly the reference constants:
// a blue sky dome plus a sharp sun lobe.
static const float3 kWorldLightSkyColor = float3(0.2, 0.5, 1.0);
static const float3 kWorldLightSunColor = float3(1.0, 0.5, 0.1) * 20.0;
static const float kWorldLightSunAngle = 5.0;
static const float kWorldLightSunSharpness = 64.0;

[ForceInline]
float3 radianceIntegrateSky_(float2 angle) {
    float a1 = angle.y;
    float a0 = angle.x;
    float sunSqrt = sqrt(kWorldLightSunSharpness);
    float3 result = kWorldLightSkyColor * (a1 - a0 - 0.5 * (cos(a1) - cos(a0)));
    result += kWorldLightSunColor *
              (atan(sunSqrt * (kWorldLightSunAngle - a0)) - atan(sunSqrt * (kWorldLightSunAngle - a1))) / sunSqrt;
    return result / 6.0;
}

// Integrates over [angle.x, angle.y); an interval crossing 2*pi is split at the
// boundary (the reference's wrap branch).
[ForceInline]
float3 radianceIntegrateSky(float2 angle) {
    if (angle.y < kTau) {
        return radianceIntegrateSky_(angle);
    }
    return radianceIntegrateSky_(float2(angle.x, kTau)) + radianceIntegrateSky_(float2(0.0, angle.y - kTau));
}

// One coarse-bin sub-interval of the sky, normalized by its angular width so a
// whole-circle average of the bins is skyIntensity * the mean sky radiance. The
// phase comes from the solar hour; a negative hour disables the sky entirely.
[ForceInline]
float3 angularSkyRadiance(uint parentNumDirs, uint parentPhi, float skyHour, float skyIntensity) {
    if (skyHour < 0.0) {
        return float3(0.0, 0.0, 0.0);
    }

    float phase = fract(skyHour / 24.0);
    float lo = 1.0 - (float(parentPhi) + 0.5) / float(parentNumDirs);
    float hi = 1.0 - (float(parentPhi) + 1.5) / float(parentNumDirs);

    lo = fract(lo + phase) * kTau;
    hi = fract(hi + phase) * kTau;

    float a0 = min(lo, hi);
    float a1 = max(lo, hi);
    // The reference divides by the bin width directly. Guard the measure-zero
    // wrap case where both endpoints land on the same value instead of emitting
    // an infinity into the cascade.
    float norm = kTau * skyIntensity / max(a1 - a0, 1.0e-6);
    return radianceIntegrateSky(float2(a0, a1)) * norm;
}

// ------------------------------------------------------------------
// accumulation
// ------------------------------------------------------------------

// Traces and accumulates one direction sample of one probe into the running
// mean, writing the current radiance buffer and the sample count. The stored
// value is this level's own interval folded with its parent's stored suffix
// (worldLightParentSampleFrom), so every level holds the composite from its own
// interval to the world reach and level 0 holds the fully merged field. The
// coarsest level has no parent: it merges each of its direction bins with the
// reference's angular sky integral over the four parent bins that the next
// (virtual) cascade would cover.
[ForceInline]
void accumulateSample(uint level, uint probe, uint direction) {
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint sample = L.sampleOffset + probe * L.directions + direction;

    uint px = probe % L.probesX;
    uint py = probe / L.probesX;
    uint probeLinear = L.probeOffset + probe;
    uint frame = g_world_light_field.frame;

    float2 jitterPos = (float2(hash01(probeLinear + 1u, frame), hash01(probeLinear + 2u, frame)) - 0.5) * kSampleJitter;

    float spacing = float(L.spacing);
    float2 cellCenter = float2(float(px) + 0.5, float(py) + 0.5) * spacing + jitterPos * spacing;
    float2 origin = float2(g_world_light_field.originX, g_world_light_field.originY) +
                    cellCenter * g_world_light_field.cellMetres;

    float3 radiance;
    float transmittance;

    if (level + 1u == g_world_light_field.levelCount) {
        // Reference radianceCascadeComputeLast: average the four next-level
        // directions that this coarse bin covers, each traced over this level's
        // interval and merged with the sky over that sub-bin's angular span.
        const uint ratio = 4u;
        const uint parentNumDirs = L.directions * ratio;
        float3 radianceSum = float3(0.0, 0.0, 0.0);
        float transmittanceSum = 0.0;
        for (uint q = 0u; q < ratio; ++q) {
            uint parentPhi = direction * ratio + q;
            float subAngle = (float(parentPhi) + 0.5) * kTau / float(parentNumDirs);
            float2 subDir = float2(cos(subAngle), sin(subAngle));

            float3 nearRadiance;
            float nearTransmittance;
            traceWorldLightInterval(origin, subDir, L.nearM, L.farM, nearRadiance, nearTransmittance);

            float3 sky = angularSkyRadiance(
                parentNumDirs, parentPhi, g_world_light_field.skyHour, g_world_light_field.skyIntensity);
            radianceSum += nearRadiance + nearTransmittance * sky;
            transmittanceSum += nearTransmittance;
        }
        radiance = radianceSum / float(ratio);
        transmittance = transmittanceSum / float(ratio);
    } else {
        float jitterAngle = (hash01(probeLinear, frame) - 0.5) * kSampleJitter;
        float angle = (float(direction) + 0.5 + jitterAngle) * kTau / float(L.directions);
        float2 dir = float2(cos(angle), sin(angle));

        traceWorldLightInterval(origin, dir, L.nearM, L.farM, radiance, transmittance);
    }

    // Fold the parent's already-merged suffix into this interval so the stored
    // sample is the composite from here to the world reach. The parent is read
    // from the previous buffer: tracing may visit levels in any order, and the
    // previous frame's complete parent suffix is race-free and temporally
    // consistent. Level 0 therefore ends up fully merged and shading reads it
    // alone. The sky is already inside the parent suffix, so it is not re-added.
    // A fully attenuated own interval makes the fold a no-op (L = L_near and
    // T = 0), so the parent reads are skipped for an opaque hit.
    if (transmittance > 0.0 && level + 1u < g_world_light_field.levelCount) {
        float3 parentRadiance;
        float parentTransmittance;
        worldLightParentSampleFrom(
            g_world_light_radiance_prev, L, g_world_light_field.levels[level + 1u], origin, direction, parentRadiance,
            parentTransmittance);
        radiance += transmittance * parentRadiance;
        transmittance *= parentTransmittance;
    }

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
