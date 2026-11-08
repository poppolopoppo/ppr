// ------------------------------------------------------------------
// cascade world light field: composite sampling and material response
// ------------------------------------------------------------------
// Shading-side sampler for the temporal cascade field, shared by the field's
// evaluate entry (Task 7) and the terrain fragment shading (Task 8). The field
// stores one interval of directional radiance per level; worldLightComposite
// bilinearly gathers each level at a world-space position, maps the continuous
// direction to that level's angular bin, and composites near-to-far with the
// scalar transmittance, L = L0 + T0*(L1 + T1*(...)). worldLightDiffuse and
// worldLightSpecular reduce the composite to a cosine-weighted diffuse term and
// a reflection term with a roughness lobe. No physical quantity is clamped here;
// the shading body clamps only its final output.
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

// Reads one probe's one angular direction from the current radiance buffer,
// unpacking the RGB radiance and scalar transmittance halves. Returns false (and
// zero radiance / unit transmittance) for probes outside the level's grid, so
// sampling at the world edge fades the out-of-world contribution instead of
// wrapping.
[ForceInline]
bool worldLightProbe(WorldLightLevelGpu level, int2 probe, uint direction, out float3 radiance, out float transmittance) {
    if (probe.x < 0 || probe.y < 0 || probe.x >= int(level.probesX) || probe.y >= int(level.probesY)) {
        radiance = float3(0.0, 0.0, 0.0);
        transmittance = 1.0;
        return false;
    }

    uint sample = level.sampleOffset + (uint(probe.y) * level.probesX + uint(probe.x)) * level.directions + direction;
    WorldLightRadiance s = g_world_light_radiance_cur[sample];
    radiance = float3(float(s.radianceR), float(s.radianceG), float(s.radianceB));
    transmittance = float(s.transmittance);
    return true;
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
// position for the given direction. Probe centres sit at integer probe
// coordinates ((px + 0.5) * spacing in cells), so subtracting a half probe maps
// them to the bilinear lattice.
[ForceInline]
void worldLightLevelSample(
    WorldLightLevelGpu level, float2 position, float2 direction, out float3 radiance, out float transmittance) {
    float2 origin = float2(g_world_light_field.originX, g_world_light_field.originY);
    float spacing = float(level.spacing);

    float2 u = (position - origin) / (g_world_light_field.cellMetres * spacing);
    float2 uv = u - 0.5;
    float2 baseF = floor(uv);
    float2 frac = uv - baseF;
    int2 base = int2(baseF);

    uint directionIndex = worldLightDirectionIndex(direction, level.directions);

    float3 r00, r10, r01, r11;
    float t00, t10, t01, t11;
    worldLightProbe(level, base + int2(0, 0), directionIndex, r00, t00);
    worldLightProbe(level, base + int2(1, 0), directionIndex, r10, t10);
    worldLightProbe(level, base + int2(0, 1), directionIndex, r01, t01);
    worldLightProbe(level, base + int2(1, 1), directionIndex, r11, t11);

    float3 r0 = lerp(r00, r10, frac.x);
    float3 r1 = lerp(r01, r11, frac.x);
    float t0 = lerp(t00, t10, frac.x);
    float t1 = lerp(t01, t11, frac.x);

    radiance = lerp(r0, r1, frac.y);
    transmittance = lerp(t0, t1, frac.y);
}

// ------------------------------------------------------------------
// composite and material terms
// ------------------------------------------------------------------

// Near-to-far transmittance composite of the whole hierarchy at one position
// and direction: L = L0 + T0*(L1 + T1*(...)). Each level carries its own
// interval radiance and attenuates everything beyond it.
[ForceInline]
float3 worldLightComposite(float2 position, float2 direction) {
    float3 result = float3(0.0, 0.0, 0.0);
    float throughput = 1.0;

    uint levelCount = g_world_light_field.levelCount;
    for (uint level = 0u; level < levelCount; ++level) {
        float3 radiance;
        float transmittance;
        worldLightLevelSample(g_world_light_field.levels[level], position, direction, radiance, transmittance);

        result += throughput * radiance;
        throughput *= transmittance;
    }
    return result;
}

// Cosine-weighted diffuse over a deterministic 8-direction set. A flat (+Z)
// normal has no horizontal tilt and samples isotropically (the top-down view's
// near-isotropic term); a tilted (normal-mapped) normal weights each direction
// by its incident cosine.
[ForceInline]
float3 worldLightDiffuse(float2 position, float3 normal) {
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
        result += worldLightComposite(position, dir) * weight;
        weightSum += weight;
    }
    if (weightSum > 0.0) {
        result /= weightSum;
    }
    return result;
}

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
