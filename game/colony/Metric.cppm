module;
#include "pP/Macros.h"

export module game.colony.metric;

import engine.core;
import engine.sim;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // constants
    // ------------------------------------------------------------------

    inline constexpr u32 kFineCellsPerMetre = 3u;
    // A persisted world-metric value, not a construction constraint: wall
    // rectangles are cell-exact and no longer snap to it. Checkpoints serialise
    // it and world export prints it, so the name and value stay fixed.
    inline constexpr u32 kFineCellsPerConstructionTile = 3u;
    inline constexpr double kFineCellMetres = 1.0 / kFineCellsPerMetre;
    inline constexpr double kReferenceThicknessMetres = 1.0;
    inline constexpr double kNominalFineCellVolumeM3 = kFineCellMetres * kFineCellMetres * kReferenceThicknessMetres;
    inline constexpr double kWorldExtentMetres = static_cast<double>(sim::kWorldEdge) / kFineCellsPerMetre;

    /// Interactive rendered worlds admit this square edge. Other engine
    /// simulation extents remain available to authoring and sim APIs.
    inline constexpr u32 kRenderedWorldEdge = 1024u;

    // ------------------------------------------------------------------
    // types
    // ------------------------------------------------------------------

    struct WorldMetric {
        u32 m_cells_per_metre{kFineCellsPerMetre};
        u32 m_cells_per_tile{kFineCellsPerConstructionTile};
        double m_reference_thickness_m{kReferenceThicknessMetres};
        [[nodiscard]] bool operator==(const WorldMetric &) const noexcept = default;
    };

    struct MetrePoint {
        double m_x{};
        double m_y{};
    };

    // ------------------------------------------------------------------
    // operations
    // ------------------------------------------------------------------

    [[nodiscard]] std::error_code validateWorldMetric(const WorldMetric &metric);

    [[nodiscard]] Expected<MetrePoint> fineCellCentre(sim::GlobalCellPos position, u32 world_edge = sim::kWorldEdge);

    // The explicit one-cell scale is retained for legacy numeric fixtures.
    [[nodiscard]] Expected<sim::GlobalCellPos> metreToFineCell(
        MetrePoint position, u32 cells_per_metre = kFineCellsPerMetre, u32 world_edge = sim::kWorldEdge);

    [[nodiscard]] Expected<double> fineCellBoundaryMetres(u32 boundary, u32 world_edge = sim::kWorldEdge);

    // Exact discrete dimensions only; no rounding of authored geometry.
    [[nodiscard]] Expected<u32> discreteFineCellLength(double metres, u32 world_edge = sim::kWorldEdge);
}
