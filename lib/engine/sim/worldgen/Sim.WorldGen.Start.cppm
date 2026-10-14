module;
export module engine.sim:worldgen.start;
import :worldgen.plan;
import :chunk_grid;
import engine.core;
import std;

export namespace pP::sim {
    // ------------------------------------------------------------------
    // generation start vocabulary
    // ------------------------------------------------------------------

    inline constexpr u32 kMaxStartContourCells = 262144u;

    // subgroup: generation start
    struct GenerationStart {
        double m_spawn_x_m{}, m_spawn_y_m{}, m_floor_y_m{};
        Array<u32> m_resource_fine_cells;
    };

    // ------------------------------------------------------------------
    // start validation
    // ------------------------------------------------------------------

    /// Proves a continuous supported level route and requested headroom against
    /// shared contour triangles, including the swept actor width. The spawn
    /// centre is placed on the reconstructed floor using the actual body height.
    [[nodiscard]] Expected<GenerationStart> validateGenerationStart(
        const GenerationPlan &plan, const ChunkGrid &grid, const SharedContext &context = nullptr,
        u64 *known_scratch_peak = nullptr);
}
