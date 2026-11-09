// ------------------------------------------------------------------
// cascade world light field: composite sampling and material response
// ------------------------------------------------------------------
// Shading-side sampler for the temporal cascade field, shared by the field's
// evaluate entry (Task 7) and the terrain fragment shading (Task 8). Each level
// stores its own interval already composited with every farther interval
// (world_light_trace.slang folds the parent suffix in at trace time), so level 0
// holds the fully merged field. worldLightComposite therefore bilinearly gathers
// level 0 only at a world-space position and maps the continuous direction to
// level 0's angular bin; no per-level loop runs at shading time.
// worldLightDiffuse and worldLightSpecular reduce that merged field to a
// cosine-weighted diffuse term and a reflection term with a roughness lobe. No
// physical quantity is clamped here; the shading body clamps only its final
// output.
#ifndef PPR_WORLD_LIGHT_SAMPLE_SLANG
#define PPR_WORLD_LIGHT_SAMPLE_SLANG

#include "world_light_field.slang"

static const float kWorldLightTau = 6.28318530718;
static const uint kWorldLightDiffuseDirections = 8u;

// Snaps an angular index that falls on (or within float error of) a bin
// boundary to the upper bin, so a direction sampled via cos/sin -> atan2 maps to
// the same bin on every level. Kept far below a bin's half-width in index space.
static const float kWorldLightDirectionEpsilon = 1e-3;

// ------------------------------------------------------------------
// one probe, one direction
// ------------------------------------------------------------------

// Reads one probe's one angular direction from the supplied radiance buffer,
// unpacking the RGB radiance and scalar transmittance halves. Returns false (and
// zero radiance / unit transmittance) for probes outside the level's grid, so
// sampling at the world edge fades the out-of-world contribution instead of
// wrapping. The buffer is a parameter so the same gather serves every consumer.
[ForceInline]
bool worldLightProbeFrom(
    RWStructuredBuffer<WorldLightRadiance> radianceBuffer, WorldLightLevelGpu level, int2 probe, uint direction,
    out float3 radianceOut, out float transmittance) {
    if (probe.x < 0 || probe.y < 0 || probe.x >= int(level.probesX) || probe.y >= int(level.probesY)) {
        radianceOut = float3(0.0, 0.0, 0.0);
        transmittance = 1.0;
        return false;
    }

    uint sample = level.sampleOffset + (uint(probe.y) * level.probesX + uint(probe.x)) * level.directions + direction;
    WorldLightRadiance s = radianceBuffer[sample];
    radianceOut = float3(float(s.radianceR), float(s.radianceG), float(s.radianceB));
    transmittance = float(s.transmittance);
    return true;
}

[ForceInline]
bool worldLightProbe(WorldLightLevelGpu level, int2 probe, uint direction, out float3 radiance, out float transmittance) {
    return worldLightProbeFrom(g_world_light_radiance_cur, level, probe, direction, radiance, transmittance);
}

// ------------------------------------------------------------------
// per-level bilinear gather
// ------------------------------------------------------------------

// Maps a continuous 2D direction to its angular bin in [0, directions). The
// stored direction d is centred at (d + 0.5) * tau / directions, matching the
// trace's angular layout. The epsilon makes the boundary deterministic: a
// direction recovered from cos/sin that lands within float error of a bin edge
// still maps to the same bin on every level instead of flickering between two.
[ForceInline]
uint worldLightDirectionIndex(float2 direction, uint directions) {
    float angle = atan2(direction.y, direction.x);
    if (angle < 0.0) angle += kWorldLightTau;
    float index = angle * float(directions) / kWorldLightTau;
    return uint(clamp(floor(index + kWorldLightDirectionEpsilon), 0.0, float(directions) - 1.0));
}

// Bilinearly gathers one level's radiance and transmittance at a world-space
// position for the given direction bin. Probe centres sit at integer probe
// coordinates ((px + 0.5) * spacing in cells), so subtracting a half probe maps
// them to the bilinear lattice. A caller that already resolved the bin (the
// trace's parent fold) uses the index form directly.
[ForceInline]
void worldLightLevelSampleIndexFrom(
    RWStructuredBuffer<WorldLightRadiance> radianceBuffer, WorldLightLevelGpu level, float2 position, uint directionIndex,
    out float3 radianceOut, out float transmittance) {
    float2 origin = float2(g_world_light_field.originX, g_world_light_field.originY);
    float spacing = float(level.spacing);

    float2 u = (position - origin) / (g_world_light_field.cellMetres * spacing);
    float2 uv = u - 0.5;
    float2 baseF = floor(uv);
    float2 frac = uv - baseF;
    int2 base = int2(baseF);

    float3 r00, r10, r01, r11;
    float t00, t10, t01, t11;
    worldLightProbeFrom(radianceBuffer, level, base + int2(0, 0), directionIndex, r00, t00);
    worldLightProbeFrom(radianceBuffer, level, base + int2(1, 0), directionIndex, r10, t10);
    worldLightProbeFrom(radianceBuffer, level, base + int2(0, 1), directionIndex, r01, t01);
    worldLightProbeFrom(radianceBuffer, level, base + int2(1, 1), directionIndex, r11, t11);

    float3 r0 = lerp(r00, r10, frac.x);
    float3 r1 = lerp(r01, r11, frac.x);
    float t0 = lerp(t00, t10, frac.x);
    float t1 = lerp(t01, t11, frac.x);

    radianceOut = lerp(r0, r1, frac.y);
    transmittance = lerp(t0, t1, frac.y);
}

[ForceInline]
void worldLightLevelSampleFrom(
    RWStructuredBuffer<WorldLightRadiance> radianceBuffer, WorldLightLevelGpu level, float2 position, float2 direction,
    out float3 radianceOut, out float transmittance) {
    worldLightLevelSampleIndexFrom(
        radianceBuffer, level, position, worldLightDirectionIndex(direction, level.directions), radianceOut,
        transmittance);
}

[ForceInline]
void worldLightLevelSample(
    WorldLightLevelGpu level, float2 position, float2 direction, out float3 radiance, out float transmittance) {
    worldLightLevelSampleFrom(g_world_light_radiance_cur, level, position, direction, radiance, transmittance);
}

// ------------------------------------------------------------------
// composite and material terms
// ------------------------------------------------------------------

// Samples one level's suffix at a finer level's probe centre for a given finer
// direction: the parent's angular bins are four times finer, so the four bins
// that span the finer bin are averaged, and the parent probes are bilinearly
// gathered at that world position. This is the fold the trace applies after
// tracing a level's own interval: L = L_near + T_near * L_far and
// T = T_near * T_far with the parent's suffix as L_far/T_far.
[ForceInline]
void worldLightParentSampleFrom(
    RWStructuredBuffer<WorldLightRadiance> radianceBuffer, WorldLightLevelGpu childLevel,
    WorldLightLevelGpu parentLevel, float2 position, uint childDirection, out float3 radiance,
    out float transmittance) {
    uint ratio = max(parentLevel.directions / max(childLevel.directions, 1u), 1u);

    float3 sumRadiance = float3(0.0, 0.0, 0.0);
    float sumTransmittance = 0.0;
    for (uint index = 0u; index < ratio; ++index) {
        float3 r;
        float t;
        worldLightLevelSampleIndexFrom(
            radianceBuffer, parentLevel, position, childDirection * ratio + index, r, t);
        sumRadiance += r;
        sumTransmittance += t;
    }

    float inverse = 1.0 / float(ratio);
    radiance = sumRadiance * inverse;
    transmittance = sumTransmittance * inverse;
}

// The merged field at one position and direction. Level 0 already holds every
// level's interval folded near-to-far, so this is a single-level gather: reading
// the remaining levels here would composite already-merged data twice.
[ForceInline]
float3 worldLightCompositeFrom(RWStructuredBuffer<WorldLightRadiance> radianceBuffer, float2 position, float2 direction) {
    float3 radiance;
    float transmittance;
    worldLightLevelSampleFrom(radianceBuffer, g_world_light_field.levels[0], position, direction, radiance, transmittance);
    return radiance;
}

[ForceInline]
float3 worldLightComposite(float2 position, float2 direction) {
    return worldLightCompositeFrom(g_world_light_radiance_cur, position, direction);
}

// Cosine-weighted diffuse over a deterministic 8-direction set. A flat (+Z)
// normal has no horizontal tilt and samples isotropically (the top-down view's
// near-isotropic term); a tilted (normal-mapped) normal weights each direction
// by its incident cosine.
[ForceInline]
float3 worldLightDiffuseFrom(RWStructuredBuffer<WorldLightRadiance> radianceBuffer, float2 position, float3 normal) {
    float2 tilt = float2(normal.x, normal.y);
    float tiltLength = length(tilt);

    float3 result = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    for (uint i = 0u; i < kWorldLightDiffuseDirections; ++i) {
        // Eight directions on every other level-1 bin centre (level 1 has 16
        // bins centred at (d + 0.5) * tau / 16), so no sample lands on a level-0
        // or level-1 bin boundary and the angular index is unambiguous.
        float angle = (2.0 * float(i) + 0.5) * kWorldLightTau / 16.0;
        float2 dir = float2(cos(angle), sin(angle));

        float weight = 1.0;
        if (tiltLength > 1e-6) {
            weight = max(dot(dir, tilt / tiltLength), 0.0);
        }
        result += worldLightCompositeFrom(radianceBuffer, position, dir) * weight;
        weightSum += weight;
    }
    if (weightSum > 0.0) {
        result /= weightSum;
    }
    return result;
}

[ForceInline]
float3 worldLightDiffuse(float2 position, float3 normal) {
    return worldLightDiffuseFrom(g_world_light_radiance_cur, position, normal);
}

// ------------------------------------------------------------------
// per-cell radiance atlas
// ------------------------------------------------------------------

// One packed RGB9E5 word per native cell, row-major. The atlas pass writes it
// earlier in the same frame; the trace samples it at an opaque hit so the
// one-bounce term costs one load instead of a cascade-direction gather.
StructuredBuffer<uint> g_world_light_atlas;

// Radiance the atlas holds for the native cell containing `world`: one load
// plus the shared RGB9E5 decode. Out-of-world positions contribute nothing.
[ForceInline]
float3 worldLightAtlasRadiance(float2 world) {
    float2 origin = float2(g_world_light_field.originX, g_world_light_field.originY);
    float2 cellF = floor((world - origin) / g_world_light_field.cellMetres);
    int2 cell = int2(cellF);
    if (cell.x < 0 || cell.y < 0 || cell.x >= int(g_world_light_field.width) ||
        cell.y >= int(g_world_light_field.height)) {
        return float3(0.0, 0.0, 0.0);
    }

    uint index = uint(cell.y) * g_world_light_field.width + uint(cell.x);
    return unpackRadianceRGB9E5(g_world_light_atlas[index]);
}

// ------------------------------------------------------------------
// material response
// ------------------------------------------------------------------

// Reflection term plus a roughness lobe. A zero roughness returns the composite
// at the reflection direction; increasing roughness blends toward a small
// deterministic neighbour set spread around the reflection. A vertical (or zero)
// reflection has no in-plane component, so there is no 2D direction to sample:
// the specular term is zero rather than a NaN normalize.
[ForceInline]
float3 worldLightSpecular(float2 position, float3 reflection, float roughness) {
    float2 r = float2(reflection.x, reflection.y);
    float rLength = length(r);
    if (rLength <= 1e-6) {
        return float3(0.0, 0.0, 0.0);
    }
    r /= rLength;

    float3 sharp = worldLightComposite(position, r);

    float lobe = clamp(roughness, 0.0, 1.0);
    if (lobe <= 0.0) {
        return sharp;
    }

    const uint count = 4u;
    float3 wide = float3(0.0, 0.0, 0.0);
    for (uint i = 0u; i < count; ++i) {
        float offset = (float(i) - 0.5 * (float(count) - 1.0)) * lobe * 0.5;
        float c = cos(offset);
        float s = sin(offset);
        float2 dir = float2(r.x * c - r.y * s, r.x * s + r.y * c);
        wide += worldLightComposite(position, dir);
    }
    wide /= float(count);
    return lerp(sharp, wide, lobe);
}

#endif
