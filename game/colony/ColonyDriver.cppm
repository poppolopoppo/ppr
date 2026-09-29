export module game.colony.driver;

import engine.core;
import engine.physics;
import engine.sim;
import game.colony;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.translator;

import std;

export namespace pP::colony {
    struct ColonyDriverDesc {
        u64 m_seed{1234567u};
        u32 m_tick_hz{60u};
    };

    /// Owns a single generated colony and its fixed-step clock. Rendering and
    /// input remain with the caller; elapsed time and regeneration seeds are
    /// supplied explicitly at the boundary.
    /// Transient physics handle (rebuilt on spawn/load; never snapshotted).
    struct AgentBody {
        sim::Entity m_entity{};
        u64 m_key{};
        physics::BodyHandle m_handle{};
        float m_velocity_x{};
        float m_velocity_y{};
    };

    class ColonyDriver {
    private:
        Colony m_colony{};
        std::optional<sim::FixedTimestep> m_timestep{};
        u64 m_seed{};
        u64 m_tick_count{};
        u32 m_speed{1u};
        bool m_paused{false};
        sim::Registry m_registry{};
        sim::StepRegistry m_steps{};
        Pathfinder m_finder{};
        Array<sim::ChunkPos> m_edit_dirty{};
        Array<sim::ChunkPos> m_collider_covered{};
        PathCounts m_counts{};
        u32 m_errands{};
        physics::Scene m_scene{};
        physics::ChunkColliders m_colliders{};
        Array<AgentBody> m_bodies{};
        Array<AgentSummary> m_agents{};
        std::error_code m_step_error{};
        const char *m_step_stage{""};
        bool m_steps_bound{false};
        bool m_physics_ready{false};

    private:
        std::error_code initPhysics();

        void shutdownPhysics() noexcept;

        std::error_code spawnColonyAgents();

        std::error_code createAgentBodies();

        void refreshAgents();

        void moveAgents() noexcept;

        void stepPhysics() noexcept;

        void mirrorBodyStates() noexcept;

        std::error_code rebuildChunks(std::span<const sim::ChunkPos> dirty, std::span<const sim::ChunkPos> needed,
                                      u32 max);

    public:

    public:
        [[nodiscard]] std::error_code init(const ColonyDriverDesc &desc);

        [[nodiscard]] std::error_code shutdown();

        /// Advances at most five fixed ticks; excess whole slices are dropped.
        [[nodiscard]] std::error_code update(TimeSpan elapsed);

        void setPaused(bool paused) noexcept;

        void togglePaused() noexcept;

        /// Only valid while initialized and paused; runs exactly one tick.
        [[nodiscard]] std::error_code stepOne() noexcept;

        /// Accepts only speeds 1, 2, or 3.
        [[nodiscard]] std::error_code setSpeed(u32 speed) noexcept;

        /// Replaces the generated world, discarding tick and time backlog.
        [[nodiscard]] std::error_code regenerate(u64 seed);

        [[nodiscard]] const sim::ChunkGrid &grid() const noexcept;

        /// Mutable grid for the explicit edit functions below; all edits
        /// must flow through them so presentation and search invalidation
        /// stay coherent.
        [[nodiscard]] sim::ChunkGrid &grid() noexcept;

        [[nodiscard]] u64 seed() const noexcept;

        [[nodiscard]] u64 tickCount() const noexcept;

        [[nodiscard]] double simMs() const noexcept;

        [[nodiscard]] bool paused() const noexcept;

        [[nodiscard]] u32 speed() const noexcept;

        [[nodiscard]] sim::Registry &registry() noexcept;

        [[nodiscard]] const sim::Registry &registry() const noexcept;

        /// Failing system of the last sticky step error ("" when clean).
        [[nodiscard]] const char *stepStage() const noexcept {
            return m_step_stage;
        }

        /// Cached counters from the last committed tick or edit (panel-safe).
        [[nodiscard]] PathCounts pathCounts() const noexcept;

        /// Live (incomplete) errands at the last committed tick or edit.
        [[nodiscard]] u32 errandCount() const noexcept;

        /// Validates and records a wall/floor/ladder placement as a
        /// progressive errand; cells apply through the `build` step.
        [[nodiscard]] Expected<sim::Entity> buildWall(const Footprint &print);

        /// Records a rectangular demolition (writes vacuum) as an errand.
        [[nodiscard]] Expected<sim::Entity> demolish(sim::GlobalCellPos min, u32 width, u32 height);

        /// Records a durable path request served by the `pathfind` step.
        [[nodiscard]] Expected<sim::Entity> requestPath(sim::GlobalCellPos from, sim::GlobalCellPos to, u32 caps);

        /// Pushes edit-touched chunks into presentation (`markChunkChanged`).
        /// Call after `update`/`stepOne` and before the translator submit.
        [[nodiscard]] std::error_code presentEdits(ColonyTranslator &translator) noexcept;

        /// Cached per-agent rows from the last committed tick (panel-safe).
        [[nodiscard]] std::span<const AgentSummary> agentSummaries() const noexcept;

        /// Rebuilds transient bodies from current components in slot order
        /// (after an external registry restore). Colliders rematerialize.
        [[nodiscard]] std::error_code restoreBodies();

        /// Replay readiness after an external grid+registry restore: rebuilds
        /// static colliders over the snapshot deltas plus the agent
        /// neighbourhood, synchronously with no budget remainder.
        [[nodiscard]] std::error_code rebuildReplayColliders(const sim::Snapshot &snapshot);

        /// Writes live body states into components, captures, and patches
        /// awake flags + replay ticks (mirror of the sim test precedent).
        [[nodiscard]] sim::Snapshot snapshotWithBodies();

        /// Collider chains over a chunk (smoke evidence).
        [[nodiscard]] std::size_t chainCount(sim::ChunkPos pos) const noexcept;

        /// Deferred collider chunks awaiting budget (smoke evidence).
        [[nodiscard]] std::size_t colliderPending() const noexcept;
    };
}
