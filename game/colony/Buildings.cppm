export module game.colony.buildings;

import engine.core;
import engine.sim;
import game.colony.pathfinding;

import std;

export namespace pP::colony {
    /// Max footprint edge in cells; bounds one errand's total work.
    inline constexpr u32 kMaxFootprintEdge = 32u;

    /// Grid cells written per BuildStep tick across all live errands.
    inline constexpr u32 kErrandCellsPerTick = 64u;

    /// Edit-domain rectangle. Game-local state (NOT sim snapshot vocabulary):
    /// solid writes `m_element`, non-solid writes vacuum; ladders are
    /// non-solid passages recorded for future climber logic (v1: no behavior).
    struct Footprint {
        sim::GlobalCellPos m_min{};
        u32 m_width{};
        u32 m_height{};
        u16 m_element{};
        bool m_solid{true};
        bool m_ladder{false};

        [[nodiscard]] constexpr bool operator==(const Footprint &) const noexcept = default;
    };

    /// One validated placement. Cells apply progressively in row-major order;
    /// `m_progress == m_total` is a done record kept for counts and debugging.
    struct BuildErrand {
        Footprint m_print{};
        u32 m_progress{};
        u32 m_total{};

        [[nodiscard]] constexpr bool operator==(const BuildErrand &) const noexcept = default;
    };

    static_assert(std::is_standard_layout_v<Footprint> and std::is_trivially_copyable_v<Footprint>);
    static_assert(std::is_standard_layout_v<BuildErrand> and std::is_trivially_copyable_v<BuildErrand>);
    static_assert(sizeof(Footprint) == 20u);
    static_assert(sizeof(BuildErrand) == 28u);

    /// Registers Footprint then BuildErrand. Never call alone: use
    /// `registerColonyComponents` so ids stay append-only and central.
    [[nodiscard]] std::error_code registerBuildingComponents(sim::Registry &registry);

    /// Single central registration in fixed order: PathReq, PathComp,
    /// Footprint, BuildErrand. Call on every live and restored registry
    /// before use; idempotent. Never insert before these entries.
    [[nodiscard]] std::error_code registerColonyComponents(sim::Registry &registry);

    /// Validates a footprint: in-world rect, edges in [1, kMaxFootprintEdge],
    /// known element, ladders non-solid.
    [[nodiscard]] std::error_code validateFootprint(const Footprint &print) noexcept;

    /// Advances every incomplete errand row-major up to `cell_budget` cells
    /// total: setCell (temperature preserved) + sim markDirty + touched-chunk
    /// record for presentation sync + Pathfinder scratch reset on any write.
    /// Structural mutation of errand rows happens only between view passes.
    void advanceErrands(sim::ChunkGrid &grid, sim::Registry &registry, Pathfinder &finder,
                        Array<sim::ChunkPos> &touched, u32 cell_budget);

    /// Live (incomplete) errand rows.
    [[nodiscard]] u32 liveErrandCount(sim::Registry &registry);
}
