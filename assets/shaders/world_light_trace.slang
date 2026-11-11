// ------------------------------------------------------------------
// cascade world light field: level-interval trace and temporal update
// ------------------------------------------------------------------
// One thread per (probe, direction) for both the cursor and dirty-first
// reprocess. Non-coarsest samples trace four per-parent-corner connectors through
// the dense source (cell ids + material palette + overlay) with a cell-by-cell
// DDA, merge each connector with its parent bin suffix
// (L = L_near + T_near * L_far), accumulate the weighted corner result into the
// running mean, and write the current radiance buffer. Level 0 therefore holds
// the fully merged field. Levels are scheduled coarsest-to-finest with a barrier
// between them, so the parent bins are read from the current write buffer; the
// same buffer also supplies the same-sample temporal-history input after the
// frame's published copy and any dirty repair. The coarsest level has no parent:
// it merges each bin with the reference angular sky integral over the four
// next-level directions it covers. An opaque hit adds one
// bounce of the per-cell radiance atlas weighted by the hit material's albedo, so
// the field accumulates primary transport plus a single material-weighted bounce
// (no recursive ray tree).
#include "world_light_field.slang"
#include "world_light_sample.slang"

static const float kTau = 6.28318530718;

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
// mean, writing the current radiance buffer and the sample count. Non-coarsest
// levels store the weighted per-parent-corner connector fold
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

    float2 cellJitter = worldLightCellJitter(frame);
    float spacing = float(L.spacing);
    float2 cellCenter = float2(float(px) + 0.5, float(py) + 0.5) * spacing + cellJitter;
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
        float angle = (float(direction) + 0.5) * kTau / float(L.directions);
        float2 dir = float2(cos(angle), sin(angle));

        // One global cell-space jitter drives the child position and every parent
        // corner endpoint. No angular jitter remains. Levels are scheduled
        // coarsest-to-fine with a barrier between them, so the helper reads the
        // current, already-written parent suffix. The four per-corner connector
        // rays replace both the old own-interval trace and the separate
        // parent-suffix fold; there is no second interval trace.
        worldLightParentSampleFrom(
            g_world_light_radiance_cur, L, g_world_light_field.levels[level + 1u], origin, cellJitter, dir,
            direction, radiance, transmittance);
    }

    // Temporal history for this same sample comes from the current write buffer,
    // which started as the published copy and may already hold this frame's dirty
    // repair. A dispatch owns each sample exclusively, and the dirty/ordinary
    // passes are separated by a global barrier, so a repeated trace sees the
    // freshly rebuilt value instead of resurrecting stale previous-frame history.
    WorldLightRadiance previous = g_world_light_radiance_cur[sample];
    float3 previousRadiance = float3(previous.radianceR, previous.radianceG, previous.radianceB);
    float previousTransmittance = float(previous.transmittance);

    uint count = g_world_light_state[sample];
    uint newCount = count == 0xffffffffu ? count : count + 1u;

    // Count is diagnostic metadata only. Coarse levels overwrite raw traced
    // results. Level 0 alone applies the mature exp(-4*dt) EMA when it already
    // has history; count zero is the dirty/fresh sentinel and replaces
    // immediately.
    float blend = 1.0;
    if (level == 0u && count > 0u) {
        const float retention = exp(-4.0 * g_world_light_field.frameDelta);
        blend = 1.0 - retention;
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
// ordinary packed-group selector
// ------------------------------------------------------------------

[ForceInline]
uint nextPow2_(uint value) {
    uint result = 1u;
    while (result < value) {
        result <<= 1u;
    }
    return result;
}

[ForceInline]
uint log2Exact_(uint value) {
    uint bits = 0u;
    uint remaining = value;
    while (remaining > 1u) {
        remaining >>= 1u;
        ++bits;
    }
    return bits;
}

[ForceInline]
uint reverseBits_(uint value, uint bits) {
    uint result = 0u;
    for (uint i = 0u; i < bits; ++i) {
        result = (result << 1u) | (value & 1u);
        value >>= 1u;
    }
    return result;
}

[numthreads(128, 1, 1)]
void updateWorldLightField(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_world_light_dispatch.groupCount) return;

    uint level = g_world_light_dispatch.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint probeSize = 2u << level;
    uint groupWidth = L.probesX * probeSize / 2u;
    uint groupHeight = L.probesY * probeSize / 2u;
    uint padX = nextPow2_(max(groupWidth, 1u));
    uint padY = nextPow2_(max(groupHeight, 1u));

    uint ordinal = g_world_light_dispatch.groupBegin + id.x;
    uint bitsX = log2Exact_(padX);
    uint bitsY = log2Exact_(padY);
    uint reversed = reverseBits_(ordinal, bitsX + bitsY);
    uint groupX = reversed % padX;
    uint groupY = reversed / padX;

    uint group = groupY * padX + groupX;
    // The exact reference hash is not available in this seam; the accepted
    // scheduling adaptation uses the established project hash32 for the offset.
    uint off = (hash32(group) + g_world_light_dispatch.phase) & 3u;
    uint pixelX = groupX * 2u + (off & 1u);
    uint pixelY = groupY * 2u + (off >> 1u);
    uint probeX = pixelX / probeSize;
    uint probeY = pixelY / probeSize;
    if (probeX >= L.probesX || probeY >= L.probesY) return;

    uint phiX = pixelX % probeSize;
    uint phiY = pixelY % probeSize;
    uint direction = phiY * probeSize + phiX;
    uint probe = probeY * L.probesX + probeX;
    accumulateSample(level, probe, direction);
}

// ------------------------------------------------------------------
// dirty-first update entry
// ------------------------------------------------------------------

[numthreads(128, 1, 1)]
void updateWorldLightFieldProbe(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_world_light_probe_update.sampleCount) return;

    uint level = g_world_light_probe_update.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint rectWidth = g_world_light_probe_update.rectWidth;
    if (rectWidth == 0u) return;

    // One thread per direction sample. Probe order is row-major inside the dirty
    // rectangle, matching the CPU's rect-linear sample count and cost accounting.
    uint probeLinear = id.x / L.directions;
    uint direction = id.x % L.directions;
    uint px = g_world_light_probe_update.probeX0 + (probeLinear % rectWidth);
    uint py = g_world_light_probe_update.probeY0 + (probeLinear / rectWidth);
    if (px >= L.probesX || py >= L.probesY) return;

    uint probe = py * L.probesX + px;
    accumulateSample(level, probe, direction);
}

// ------------------------------------------------------------------
// composite evaluate (test seam)
// ------------------------------------------------------------------

// One-shot sampler evaluation for a supplied position/direction/normal. The mode
// selects composite (0), diffuse (1), specular (2), irradiance (3) or the
// level-0/level-1 parent connector helper (4); the RGB result is written to
// g_world_light_evaluate_out. Test-only readback path.
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
    } else if (g_world_light_evaluate.mode == 3u) {
        result = worldLightIrradiance(position, normal);
    } else if (g_world_light_evaluate.mode == 4u) {
        uint childDirections = max(g_world_light_field.levels[0].directions, 1u);
        uint childDirection = worldLightDirectionIndex(direction, childDirections);
        float childAngle = (float(childDirection) + 0.5) * kTau / float(childDirections);
        float2 childDir = float2(cos(childAngle), sin(childAngle));
        float2 cellJitter = worldLightCellJitter(g_world_light_field.frame);
        float2 jitteredPosition =
            position + cellJitter * g_world_light_field.cellMetres;
        float parentTransmittance;
        worldLightParentSampleFrom(
            g_world_light_radiance_cur, g_world_light_field.levels[0], g_world_light_field.levels[1],
            jitteredPosition, cellJitter, childDir, childDirection, result, parentTransmittance);
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

[numthreads(128, 1, 1)]
void resetWorldLightField(uint3 id : SV_DispatchThreadID) {
    uint rectWidth = g_world_light_reset.rectWidth;
    uint rectHeight = g_world_light_reset.rectHeight;
    if (rectWidth == 0u || rectHeight == 0u) return;

    uint level = g_world_light_reset.level;
    WorldLightLevelGpu L = g_world_light_field.levels[level];
    uint sampleCount = rectWidth * rectHeight * L.directions;
    if (id.x >= sampleCount) return;

    // One thread per direction sample clears counts only. Never clearing the
    // radiance buffers is what keeps the last published value visible until the
    // invalidation backlog reaches this sample again.
    uint probeLinear = id.x / L.directions;
    uint direction = id.x % L.directions;
    uint px = g_world_light_reset.probeX0 + (probeLinear % rectWidth);
    uint py = g_world_light_reset.probeY0 + (probeLinear / rectWidth);
    if (px >= L.probesX || py >= L.probesY) return;

    uint probe = py * L.probesX + px;
    uint sample = L.sampleOffset + probe * L.directions + direction;
    g_world_light_state[sample] = 0u;
}
