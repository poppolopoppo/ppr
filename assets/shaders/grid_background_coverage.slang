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

bool backgroundCoveredByOpaqueTerrain(float2 fine_position) {
    if ((g_background_coverage.availability & kGridFrameHasResolvedCells) == 0u ||
        g_background_coverage.diagnostic != 0u) return false;
    if (!all(isfinite(fine_position)) || any(fine_position < 0.0) || any(fine_position >= 1024.0)) return false;

    uint2 cell = uint2(floor(fine_position));
    uint index = cell.y * 1024u + cell.x;
    uint material = unpackCellMaterial(g_resolved_cells[index]);
    if (material == 0u) return false;
    if ((g_background_coverage.availability & kGridFrameHasParticleOwnership) != 0u &&
        particleOwnsCell(index, g_best_row)) return false;

    TerrainVisual visual = g_terrain_visuals[min(material, 65535u)];
    return visual.material == material && (visual.surfaceFlags & kOpticalBlendSurfaceFlag) == 0u;
}

#endif
