export module game.colony.agents;

import engine.core;
import engine.sim;
import game.colony.buildings;
import game.colony.pathfinding;

import std;

export namespace pP::colony {
    /// Agent count spawned per colony (deterministic, seed-derived).
    inline constexpr u32 kAgentCount = 3u;

    /// Profiling ceiling for the spawn-count override (Slice 6 fixture).
    /// Requests above this fail with `value_too_large`; requests of 0 fail
    /// closed to `kAgentCount` (never zero agents).
    inline constexpr u32 kAgentCountMax = 128u;

    /// Default walk speed in cells per second.
    inline constexpr float kAgentSpeed = 3.0f;

    /// Waypoint capture radius in cells.
    inline constexpr float kWaypointRadius = 0.5f;

    /// Needs thresholds selecting Rest / fresh-air goals.
    inline constexpr float kRestThreshold = 30.0f;
    inline constexpr float kO2Threshold = 30.0f;

    /// Plan opcodes (movement always resolves through the agent's own
    /// PathReq/PathComp pair; work/rest complete by presence at the site).
    inline constexpr u32 kOpMove = 0u;
    inline constexpr u32 kOpWork = 1u;
    inline constexpr u32 kOpRest = 2u;

    /// High-level plan actions per Plan (waypoints live in PathComp).
    inline constexpr u32 kMaxPlanActions = 8u;

    enum class EGoal : u32 { seek, work, flee };

    struct AgentAction {
        u32 m_op{kOpMove};
        sim::GlobalCellPos m_target{};

        [[nodiscard]] constexpr bool operator==(const AgentAction &) const noexcept = default;
    };

    /// Durable agent identity + locomotion params + seed-derived spawn.
    struct Agent {
        u64 m_key{};
        float m_speed{kAgentSpeed};
        sim::GlobalCellPos m_spawn{};

        [[nodiscard]] constexpr bool operator==(const Agent &) const noexcept = default;
    };

    struct Needs {
        float m_o2{100.0f};
        float m_rest{100.0f};

        [[nodiscard]] constexpr bool operator==(const Needs &) const noexcept = default;
    };

    /// Durable planner output. `m_replans` counts goal changes; panel reads
    /// it directly. Actions execute in order at `m_pc`.
    struct Plan {
        EGoal m_goal{EGoal::seek};
        sim::GlobalCellPos m_site{};
        AgentAction m_actions[kMaxPlanActions]{};
        u32 m_action_count{};
        u32 m_pc{};
        u32 m_replans{};

        [[nodiscard]] constexpr bool operator==(const Plan &) const noexcept = default;
    };

    /// Pure movement intent for one agent (not a component).
    struct Intent {
        float m_velocity_x{};
        float m_velocity_y{};
        bool m_climbing{false};
    };

    /// Panel-safe per-agent row (driver caches these per tick).
    struct AgentSummary {
        float m_x{};
        float m_y{};
        u32 m_goal{};
        u32 m_pc{};
        u32 m_replans{};
        bool m_has_plan{false};
    };

    static_assert(std::is_standard_layout_v<Agent> and std::is_trivially_copyable_v<Agent>);
    static_assert(std::is_standard_layout_v<Needs> and std::is_trivially_copyable_v<Needs>);
    static_assert(std::is_standard_layout_v<Plan> and std::is_trivially_copyable_v<Plan>);
    static_assert(sizeof(Agent) == 24u);
    static_assert(sizeof(Needs) == 8u);
    static_assert(sizeof(Plan) == 120u);

    /// Registers Agent, Needs, Plan (appended after the building components
    /// by the central helper; never call alone).
    [[nodiscard]] std::error_code registerAgentComponents(sim::Registry &registry);

    /// Creates `count` agents at seed-derived vacuum cells with full physics
    /// definition state. No plans yet; the planner fills them.
    /// Units: `count` is an agent count (not cells or ticks). Order:
    /// slot-ordered draws from the seed-derived splitmix64 stream, so the same
    /// seed+count reproduces the same spawns and the first N spawns of a larger
    /// run match the N-spawn run (prefix-stable). `count == 0` fails closed to
    /// `kAgentCount`; `count > kAgentCountMax` returns `value_too_large`.
    /// No wall-clock or global RNG is consulted.
    [[nodiscard]] std::error_code spawnAgents(
        const sim::ChunkGrid &grid, sim::Registry &registry, u64 seed, u32 count = kAgentCount);

    /// Plans the first agent (slot order) needing a plan: none, or desired
    /// goal/site differing from the current Plan. Issues or refreshes the
    /// agent's own PathReq; the pathfind step solves it.
    void planAgent(const sim::ChunkGrid &grid, sim::Registry &registry);

    /// Advances one agent's waypoint cursor and resolves velocity (ladder
    /// zones climb). Mutates only PathComp.m_cursor / Needs / Plan progress.
    [[nodiscard]] std::error_code agentIntent(sim::Registry &registry, sim::Entity entity, Intent &intent);
}
