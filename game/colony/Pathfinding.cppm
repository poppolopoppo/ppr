export module game.colony.pathfinding;

import engine.core;
import engine.sim;

import std;

export namespace pP::colony {
    /// Capability bits for `PathReq::m_caps`. Only plain walking exists;
    /// doors/permissions and flying are non-goals (plan §5).
    inline constexpr u32 kPathCapsWalk = 1u;

    /// Search expansions charged per fixed tick; remainder slices next tick.
    inline constexpr u32 kExpansionsPerTick = 256u;

    /// Waypoint capacity of one `PathComp`. Longer routes truncate from the
    /// goal side and report partial.
    inline constexpr u32 kMaxPathPts = 256u;

    /// Cap on visited cells per search; keeps transient scratch bounded.
    inline constexpr u32 kMaxSearchNodes = 4096u;

    /// Words of the membership bitset covering every world cell exactly once.
    inline constexpr u32 kSearchBitWords =
            (sim::kWorldEdge * sim::kWorldEdge + 63u) / 64u;

    /// Durable path request. Lives in the existing ECS columns section, so it
    /// is snapshot-covered with no format bump; registration order is fixed by
    /// `registerPathfindingComponents`.
    struct PathReq {
        sim::Entity m_entity{};
        sim::GlobalCellPos m_from{};
        sim::GlobalCellPos m_to{};
        u32 m_caps{kPathCapsWalk};

        [[nodiscard]] constexpr bool operator==(const PathReq &) const noexcept = default;
    };

    /// Durable path result. Bounded inline waypoints only — no vector, no
    /// heap — so the component stays trivially copyable and snapshot-safe.
    /// `m_pts[0]` is the first step away from the request origin (the origin
    /// itself is excluded); `m_cursor` is reserved for future movement.
    /// `m_partial` with `m_count > 0` is a best-effort prefix (budget/node-cap
    /// cut); `m_partial` with `m_count == 0` is blocked.
    struct PathComp {
        sim::GlobalCellPos m_pts[kMaxPathPts]{};
        u32 m_count{};
        u32 m_cursor{};
        bool m_partial{false};

        [[nodiscard]] constexpr bool operator==(const PathComp &) const noexcept = default;
    };

    /// Honest counters for the debug panel (which cannot import this module).
    struct PathCounts {
        u32 m_paths{};
        u32 m_partials{};
        u32 m_blocked{};
    };

    /// One visited cell: packed world index plus the node index of the cell
    /// it was generated from (`none_v` for the search origin).
    struct SearchNode {
        u32 m_packed{};
        u32 m_parent{none_v};
    };

    /// Transient per-search scratch. Driver-owned, never a component, never
    /// snapshotted; rebuilt from the durable request on load/regenerate.
    struct Pathfinder {
        bool m_has_search{false};
        sim::Entity m_entity{};
        sim::GlobalCellPos m_from{};
        sim::GlobalCellPos m_to{};
        u32 m_caps{kPathCapsWalk};
        Array<SearchNode> m_nodes{};
        Array<u32> m_frontier{};
        u32 m_head{};
        Array<u64> m_bits{};
        u32 m_closest{};
        u32 m_closest_dist{};
    };

    static_assert(std::is_standard_layout_v<PathReq> and std::is_trivially_copyable_v<PathReq>);
    static_assert(std::is_standard_layout_v<PathComp> and std::is_trivially_copyable_v<PathComp>);
    static_assert(sizeof(PathReq) == 28u);
    static_assert(sizeof(PathComp) == 2060u);

    /// Walkable means vacuum element. A never-written chunk reads back as
    /// `Cell{}` (all vacuum), so non-resident space is walkable by convention.
    /// Outside the world reads false.
    [[nodiscard]] bool isWalkable(const sim::ChunkGrid &grid, sim::GlobalCellPos pos) noexcept;

    /// Registers `PathReq` then `PathComp` in fixed order. Never call alone:
    /// use `registerColonyComponents` (game.colony.buildings) so ids stay
    /// append-only and central. Kept for the buildings module only.
    [[nodiscard]] std::error_code registerPathfindingComponents(sim::Registry &registry);

    /// Advances at most one pending request by `budget` expansions, resuming
    /// the in-progress search in `finder`. Emplaces `PathComp` on completion
    /// (goal, unreachable, blocked, or node-cap); budget exhaustion simply
    /// returns and resumes next tick. Requests serve in entity slot order.
    void stepPathfinding(const sim::ChunkGrid &grid, sim::Registry &registry, Pathfinder &finder, u32 budget);

    /// Classifies every `PathComp` row: complete (or trivially satisfied)
    /// versus partial prefix versus blocked. Unregistered module reads zero.
    [[nodiscard]] PathCounts pathCounts(sim::Registry &registry);
}
