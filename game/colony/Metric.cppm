module;

export module game.colony.metric;

import engine.core;
import engine.sim;
import std;

export namespace pP::colony {
    inline constexpr u32 kFineCellsPerMetre = 3u;
    inline constexpr u32 kFineCellsPerConstructionTile = 3u;
    inline constexpr double kFineCellMetres = 1.0 / kFineCellsPerMetre;
    inline constexpr double kReferenceThicknessMetres = 1.0;
    inline constexpr double kNominalFineCellVolumeM3 = kFineCellMetres * kFineCellMetres * kReferenceThicknessMetres;
    inline constexpr double kWorldExtentMetres = static_cast<double>(sim::kWorldEdge) / kFineCellsPerMetre;

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
    struct ConstructionTile {
        u32 m_x{};
        u32 m_y{};
    };
    struct TileExtent {
        u32 m_width{1u};
        u32 m_height{1u};
    };
    // Half-open global fine-cell rectangle; a footprint may cross chunk edges.
    struct FineCellRect {
        sim::GlobalCellPos m_begin{};
        sim::GlobalCellPos m_end{};
    };

    [[nodiscard]] std::error_code validateWorldMetric(const WorldMetric &metric);
    [[nodiscard]] Expected<MetrePoint> fineCellCentre(sim::GlobalCellPos position);
    // The explicit one-cell scale is retained for legacy numeric fixtures.
    [[nodiscard]] Expected<sim::GlobalCellPos> metreToFineCell(
        MetrePoint position, u32 cells_per_metre = kFineCellsPerMetre);
    [[nodiscard]] Expected<double> fineCellBoundaryMetres(u32 boundary);
    // Exact discrete dimensions only; no rounding of authored geometry.
    [[nodiscard]] Expected<u32> discreteFineCellLength(double metres);
    [[nodiscard]] Expected<FineCellRect> constructionFootprint(ConstructionTile tile, TileExtent extent = {});
    // The incomplete final row/column cannot select a construction tile.
    [[nodiscard]] Expected<ConstructionTile> constructionTileOf(sim::GlobalCellPos position);
}
