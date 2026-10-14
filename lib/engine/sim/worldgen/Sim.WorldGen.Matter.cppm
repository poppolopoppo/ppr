module;
export module engine.sim:worldgen.matter;
import :worldgen.geometry;
import :chunk_grid;
import engine.core;
import std;

export namespace pP::sim {

    // ------------------------------------------------------------------
    // initial matter vocabulary
    // ------------------------------------------------------------------

    inline constexpr u32 kMaxInitialMatterRegions = 4096u;
    inline constexpr u32 kMaxInitialMatterSpans = 1024u * 1024u;
    inline constexpr u32 kMaxMatterComponentCells = 1024u * 1024u;
    inline constexpr u32 kMaxMatterSearchCells = 4u * 1024u * 1024u;

    struct InitialMatterSpan {
        u32 m_y{}, m_begin_x{}, m_end_x{}; // Exclusive end; global fine cells.
        constexpr bool operator==(const InitialMatterSpan &) const noexcept = default;
    };

    struct InitialMatterRegion {
        u64 m_identity{};
        u16 m_material{};
        double m_temperature_k{}, m_bulk_density_kg_m3{}, m_volume_m3{}, m_mass_kg{};
        std::optional<double> m_fill_height_m{}; // Global horizontal boundary.
        Array<InitialMatterSpan> m_spans;
    };

    struct InitialMatter {
        Array<InitialMatterRegion> m_regions;
        u32 m_span_count{}, m_searched_cells{}, m_largest_component{};
    };

    /// Exact empty area in a nominal fine-cell box using the shared midpoint
    /// contour triangles, optionally clipped below a global metre height.
    /// Terrain outside the world is closed solid.
    [[nodiscard]] Expected<double> generationEmptyArea(
        const ChunkGrid &grid, GlobalCellPos cell, double cell_size_m,
        std::optional<double> fill_height_m = std::nullopt);

    /// Geometry belongs to this plan; terrain is its final, immutable candidate.
    /// Ordered fills emit disjoint spans over zero-terrain cells without changing
    /// the grid. All scratch and output are owned by this invocation/result.
    [[nodiscard]] Expected<InitialMatter> resolveInitialMatter(
        const GenerationPlan &plan, const GenerationGeometry &geometry, const ChunkGrid &grid,
        const SharedContext &context = nullptr, u64 *known_scratch_peak = nullptr);
}
