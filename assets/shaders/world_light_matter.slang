// ------------------------------------------------------------------
// dense Matter overlay projection: clear, scatter, finalize
// ------------------------------------------------------------------
// The dense Matter projection scatters raw particles directly on the GPU (no
// host sort/unique): each addressed cell is cleared, the particles are
// atomically accumulated (opaque fill -> coverage; participating fill -> fill *
// sigma/emission), then each addressed cell is finalized (coverage clamped,
// negative terms dropped). Terrain optics merging is deferred to the field
// tracer.
#include "world_light_source.slang"

struct WorldLightMatterParticle {
    uint cell;
    float fill;
    uint material;
};

StructuredBuffer<WorldLightMatterParticle> g_world_light_particles;
StructuredBuffer<WorldLightMaterial> g_world_light_materials;

// Atomic float accumulation via a compare-exchange loop over the u32 bit
// pattern. The clear pass zeroes the addressed cells first, so accumulation
// starts from an established baseline.
[ForceInline]
void atomicAddF32(RWStructuredBuffer<uint> buffer, uint index, float value) {
    if (value == 0.0f) return;

    uint expected;
    InterlockedAdd(buffer[index], 0u, expected);
    while (true) {
        float next = asfloat(expected) + value;
        uint previous;
        InterlockedCompareExchange(buffer[index], expected, asuint(next), previous);
        if (previous == expected) break;
        expected = previous;
    }
}

// Zeroes the dense overlay cell addressed by each staged particle. This is the
// "clear the dirty cell region" step: a zeroed row clears its contribution, and
// previously occupied cells removed from this update keep their prior value
// untouched (only the addressed cells are cleared).
[shader("compute")]
[numthreads(64, 1, 1)]
void clearWorldLightMatter(uint3 id : SV_DispatchThreadID) {
    uint record = id.x;
    if (record >= g_world_light_source.hasOverlay) return;

    uint cell = g_world_light_particles[record].cell;
    uint cellCount = g_world_light_source.width * g_world_light_source.height;
    if (cell >= cellCount) return;

    uint base = cell * kOverlayStride;
    for (uint k = 0u; k < kOverlayStride; ++k) {
        g_world_light_overlay[base + k] = 0u;
    }
}

// Atomically accumulates each particle into its addressed cell. Opaque material
// (kind 1) contributes its fill to coverage; participating material (kind 2)
// contributes fill-weighted sigma/emission.
[shader("compute")]
[numthreads(64, 1, 1)]
void scatterWorldLightMatter(uint3 id : SV_DispatchThreadID) {
    uint record = id.x;
    if (record >= g_world_light_source.hasOverlay) return;

    WorldLightMatterParticle particle = g_world_light_particles[record];
    uint cellCount = g_world_light_source.width * g_world_light_source.height;
    if (particle.cell >= cellCount) return;

    WorldLightMaterial material = g_world_light_materials[particle.material];
    uint base = particle.cell * kOverlayStride;
    if (material.kind == 1u) {
        atomicAddF32(g_world_light_overlay, base + kOverlayCoverage, particle.fill);
    } else if (material.kind == 2u) {
        atomicAddF32(g_world_light_overlay, base + kOverlaySigma0, particle.fill * material.sigma[0]);
        atomicAddF32(g_world_light_overlay, base + kOverlaySigma1, particle.fill * material.sigma[1]);
        atomicAddF32(g_world_light_overlay, base + kOverlaySigma2, particle.fill * material.sigma[2]);
        atomicAddF32(g_world_light_overlay, base + kOverlayEmission0, particle.fill * material.emission[0]);
        atomicAddF32(g_world_light_overlay, base + kOverlayEmission1, particle.fill * material.emission[1]);
        atomicAddF32(g_world_light_overlay, base + kOverlayEmission2, particle.fill * material.emission[2]);
    }
}

// Finalizes each addressed cell: clamps opaque coverage to [0,1] and drops
// negative sigma/emission. Terrain optics merging is deferred to the field
// tracer. Occupancy is rebuilt afterward by buildWorldLightOccupancy.
[shader("compute")]
[numthreads(64, 1, 1)]
void finishWorldLightMatter(uint3 id : SV_DispatchThreadID) {
    uint record = id.x;
    if (record >= g_world_light_source.hasOverlay) return;

    uint cell = g_world_light_particles[record].cell;
    uint cellCount = g_world_light_source.width * g_world_light_source.height;
    if (cell >= cellCount) return;

    uint base = cell * kOverlayStride;
    float coverage = clamp(asfloat(g_world_light_overlay[base + kOverlayCoverage]), 0.0f, 1.0f);
    g_world_light_overlay[base + kOverlayCoverage] = asuint(coverage);
    for (uint k = kOverlaySigma0; k < kOverlayStride; ++k) {
        float term = max(asfloat(g_world_light_overlay[base + k]), 0.0f);
        g_world_light_overlay[base + k] = asuint(term);
    }
}
