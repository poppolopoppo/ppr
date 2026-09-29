export module game.colony.digtool;

import engine.core;
import engine.sim;

import std;

export namespace pP::colony {
    /// Max dig edge in cells; mirrors `kMaxFootprintEdge` without importing
    /// the buildings module (this module imports core/sim only).
    inline constexpr u32 kMaxDigEdge = 32u;

    /// Cell budget of one dig: the full 32x32 rect. Vacuum no-ops never
    /// charge it; oversized rects fail validation before any write.
    inline constexpr u32 kMaxDigCells = kMaxDigEdge * kMaxDigEdge;

    /// Dig rectangle. Game-local edit vocabulary (NOT sim snapshot state):
    /// every non-vacuum cell inside clears to vacuum, temperature preserved.
    struct DigRect {
        sim::GlobalCellPos m_min{};
        u32 m_width{};
        u32 m_height{};

        [[nodiscard]] constexpr bool operator==(const DigRect &) const noexcept = default;
    };

    /// Dig outcome: exact non-vacuum cells cleared plus the deduped chunks
    /// written, in row-major encounter order (at most four for a 32x32 rect).
    struct DigResult {
        u32 m_dug{};
        Array<sim::ChunkPos> m_touched{};
    };

    static_assert(std::is_standard_layout_v<DigRect>);
    static_assert(sizeof(DigRect) == 16u);

    /// Validates `rect` (edges in [1, kMaxDigEdge], area within kMaxDigCells,
    /// rect inside the world), then clears every non-vacuum cell row-major to
    /// vacuum with temperature preserved. Collects first and applies second,
    /// so validation or budget failure changes nothing. Vacuum cells are
    /// no-ops: no write, no budget, no dirt. `setCell` auto-dirties, so no
    /// explicit `markDirty` call is needed. Deterministic: no RNG, no clock.
    [[nodiscard]] Expected<DigResult> digCells(sim::ChunkGrid &grid, const DigRect &rect);
}
