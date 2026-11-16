#ifndef PPR_GRID_BACKGROUND_COVERAGE_SLANG
#define PPR_GRID_BACKGROUND_COVERAGE_SLANG

#include "world_lighting_codec.slang"
#include "grid_particle_native.slang"

// ------------------------------------------------------------------
// categorical terrain coverage of the visual background
// ------------------------------------------------------------------
StructuredBuffer<uint> g_resolved_cells;
StructuredBuffer<TerrainVisual> g_terrain_visuals;
StructuredBuffer<uint> g_best_row;

// Separate from the background's current-FineCH gate: a retained categorical
// publication and current lighting have different availability contracts.
struct BackgroundCoverage {
    uint availability;
    uint diagnostic;
    uint reserved0;
    uint reserved1;
};
ConstantBuffer<BackgroundCoverage> g_background_coverage;

// Existing GridFrame availability protocol, supplied by bindTerrainLighting_.
static const uint kGridFrameHasResolvedCells = 1u;
static const uint kGridFrameHasParticleOwnership = 2u;

// True when one native cell is owned by opaque terrain: a nonzero resolved
// material, the palette identity guard, no optical blend, and not a
// particle-owned cell.
bool backgroundCellCovered(uint2 cell) {
    if (any(cell >= uint2(1024u, 1024u))) return false;

    uint index = cell.y * 1024u + cell.x;
    uint material = unpackCellMaterial(g_resolved_cells[index]);
    if (material == 0u) return false;
    if ((g_background_coverage.availability & kGridFrameHasParticleOwnership) != 0u &&
        particleOwnsCell(index, g_best_row)) return false;

    TerrainVisual visual = g_terrain_visuals[min(material, 65535u)];
    return visual.material == material && (visual.surfaceFlags & kOpticalBlendSurfaceFlag) == 0u;
}

// True when the whole 3x3 cell neighbourhood is opaque terrain, so the caller may
// skip the background for this sample.
//
// The neighbourhood is not decoration. This predicated skip assumes the terrain
// pass will paint the sample afterwards, but the two derive the cell through
// different arithmetic: the background unprojects the sample through the inverse
// view-projection, while the terrain interpolates `fine_position` as a varying.
// On a cell boundary the two can land in different cells, so a single-cell test
// discards the background where the terrain then discards too and the sample
// keeps the previous frame's contents (visible as isolated bright speckles on a
// dark scene). Requiring all nine cells makes the skip sound whichever side of a
// boundary either derivation lands on; a boundary sample then keeps its
// background, which costs a one-cell ring of extra work and nothing else. Do not
// narrow this back to a single cell.
//
// The neighbourhood bounds the derivation difference, not the terrain's own
// coverage: the terrain is a contour renderer, so a covered cell can still leave
// individual samples unpainted. Widening further only trades work for that
// remainder.
bool backgroundCoveredByOpaqueTerrain(float2 fine_position) {
    if ((g_background_coverage.availability & kGridFrameHasResolvedCells) == 0u ||
        g_background_coverage.diagnostic != 0u) return false;
    if (!all(isfinite(fine_position)) || any(fine_position < 0.0) || any(fine_position >= 1024.0)) return false;

    int2 base = int2(floor(fine_position));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 neighbour = base + int2(dx, dy);
            if (any(neighbour < int2(0)) || any(neighbour >= int2(1024))) return false;
            if (!backgroundCellCovered(uint2(neighbour))) return false;
        }
    }

    return true;
}

#endif
