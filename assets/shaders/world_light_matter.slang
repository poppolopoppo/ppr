// ------------------------------------------------------------------
// dense Matter overlay projection: clear, scatter, finalize
// ------------------------------------------------------------------
// The simplified dense Matter projection replaces the sparse particle
// projection's radix sort, hash lookup, ancestor buffers and old/new diff with
// three passes over the dense overlay. Each addressed cell is zeroed, then the
// caller's already-resolved per-cell records (WorldLightOverlayCell) are
// written back, then finalized (coverage clamped, negative terms dropped).
// Terrain optics merging is deferred to the field tracer.
#include "world_light_source.slang"

struct WorldLightOverlayCell {
    uint cell;
    float coverage;
    float sigma[3];
    float emission[3];
};

StructuredBuffer<WorldLightOverlayCell> g_world_light_particles;

// Zeroes the dense overlay cell addressed by each staged record. This is the
// "clear the dirty cell region" step: a zeroed row clears its contribution,
// and previously occupied cells removed from this update keep their prior
// value untouched (only the addressed cells are cleared).
[shader("compute")]
[numthreads(64, 1, 1)]
void clearWorldLightMatter(uint3 id : SV_DispatchThreadID) {
    uint record = id.x;
    if (record >= g_world_light_source.hasOverlay) return;

    uint cell = g_world_light_particles[record].cell;
    uint cellCount = g_world_light_source.width * g_world_light_source.height;
    if (cell >= cellCount) return;

    WorldLightOverlayRow zero = (WorldLightOverlayRow)0;
    g_world_light_overlay[cell] = zero;
}

// Writes each record's per-cell coverage/sigma/emission into its addressed
// cell. Records are already coalesced per cell on the host (the CPU source
// applies last-wins overwrites), so one thread owns each addressed cell and no
// cross-thread atomic accumulation is required.
[shader("compute")]
[numthreads(64, 1, 1)]
void scatterWorldLightMatter(uint3 id : SV_DispatchThreadID) {
    uint record = id.x;
    if (record >= g_world_light_source.hasOverlay) return;

    WorldLightOverlayCell particle = g_world_light_particles[record];
    uint cellCount = g_world_light_source.width * g_world_light_source.height;
    if (particle.cell >= cellCount) return;

    WorldLightOverlayRow row = (WorldLightOverlayRow)0;
    row.coverage = particle.coverage;
    row.sigma[0] = particle.sigma[0];
    row.sigma[1] = particle.sigma[1];
    row.sigma[2] = particle.sigma[2];
    row.emission[0] = particle.emission[0];
    row.emission[1] = particle.emission[1];
    row.emission[2] = particle.emission[2];
    g_world_light_overlay[particle.cell] = row;
}

// Finalizes each addressed cell: clamps opaque coverage to [0,1] and drops
// negative sigma/emission. Terrain optics merging is deferred to the field
// tracer, so the overlay retains only the projected particle terms here.
[shader("compute")]
[numthreads(64, 1, 1)]
void finishWorldLightMatter(uint3 id : SV_DispatchThreadID) {
    uint record = id.x;
    if (record >= g_world_light_source.hasOverlay) return;

    uint cell = g_world_light_particles[record].cell;
    uint cellCount = g_world_light_source.width * g_world_light_source.height;
    if (cell >= cellCount) return;

    WorldLightOverlayRow row = g_world_light_overlay[cell];
    row.coverage = clamp(row.coverage, 0.0f, 1.0f);
    row.sigma[0] = max(row.sigma[0], 0.0f);
    row.sigma[1] = max(row.sigma[1], 0.0f);
    row.sigma[2] = max(row.sigma[2], 0.0f);
    row.emission[0] = max(row.emission[0], 0.0f);
    row.emission[1] = max(row.emission[1], 0.0f);
    row.emission[2] = max(row.emission[2], 0.0f);
    g_world_light_overlay[cell] = row;
}
