module;
#include "StageTiming.h"

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
    /// Wake radius around dug chunks in cells (one physics world unit covers
    /// one cell, so the wake predicate compares cell-space distances directly).
    inline constexpr u32 kDigWakeCells = 8u;

    /// Cached dig outcomes for panel/smoke reads. Reset on regenerate,
    /// restore, replay rebuild, and shutdown. `m_wake_radius` is fixed.
    struct DigStats {
        u32 m_dug_total{};
        const u32 m_wake_radius{kDigWakeCells};
        Array<sim::ChunkPos> m_last_chunks{};
    };

    struct ColonyDriverDesc {
        u64 m_seed{1234567u};
        u32 m_tick_hz{60u};
        /// Spawn-count override for profiling fixtures (agent count, not cells
        /// or ticks). 0 fails closed to `kAgentCount`; values above
        /// `kAgentCountMax` reject init/regenerate with `value_too_large`.
        /// Production default stays `kAgentCount` (3).
        u32 m_agent_count{kAgentCount};
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
        struct PendingDig {
            sim::GlobalCellPos m_min{};
            u32 m_width{};
            u32 m_height{};
        };

        Colony m_colony{};
        std::optional<sim::FixedTimestep> m_timestep{};
        u64 m_seed{};
        u64 m_tick_count{};
        u32 m_agent_count{kAgentCount};
        u32 m_speed{1u};
        bool m_paused{false};
        sim::Registry m_registry{};
        sim::StepRegistry m_steps{};
        Pathfinder m_finder{};
        Array<sim::ChunkPos> m_present_dirty{};
        Array<sim::ChunkPos> m_collider_dirty{};
        DigStats m_dig_stats{};
        std::optional<PendingDig> m_pending_dig{};
        Array<sim::ChunkPos> m_collider_covered{};
        /// Collider view cache (H1 Slice 6: split expensive preparation from
        /// the cheap per-tick drain). Key = sorted `needed` array +
        /// `m_cover_gen` + validity; values = prepared listed/pool/views.
        /// H3a Slice 6 invariant: cached views are always a SUPERSET of what
        /// drain needs — clearing (or retaining) `m_collider_covered` removes
        /// views rather than changing content, so it must NOT bump
        /// `m_cover_gen`; only real content change bumps (non-empty absorb in
        /// fanOutEditChunks, covered growth, restore/reinit invalidations).
        /// When pending==0 the drain contract (pending ⊆ views) holds
        /// trivially regardless of covered; new dirt bumps gen via the absorb
        /// path → guaranteed MISS → views rebuilt including the new chunks.
        /// DERIVED state only: never snapshotted, never compared. The views
        /// borrow the pool, so the pool member precedes the views member.
        u64 m_cover_gen{};
        u64 m_cached_gen{};
        bool m_cache_valid{false};
        Array<sim::ChunkPos> m_cached_needed{};
        Array<sim::ChunkPos> m_cached_listed{};
        Array<Array<u16> > m_cached_pool{};
        Array<physics::ColliderChunkView> m_cached_views{};
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
        StageTimings m_timings{};

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

        void applyToolEdits() noexcept;

        [[nodiscard]] std::error_code applyDig(const PendingDig &pending);

        [[nodiscard]] std::error_code wakeBodiesNearEdit(std::span<const sim::ChunkPos> touched) noexcept;

        void fanOutEditChunks(std::span<const sim::ChunkPos> touched);

        void clearToolState() noexcept;

        void invalidateColliderCache() noexcept;

        /// H4a Slice 6: incremental view maintenance for a cache MISS. Diffs
        /// `sorted_needed` against `m_cached_needed`, closes the new listed
        /// set with an indexed (bitset) coverage merge, copies ONLY the recopy
        /// set (added ∪ dirty ∪ coverage-growth) fresh, and moves kept buffers
        /// verbatim. Returns false when the full preparation must run instead
        /// (empty history, inconsistent sizes, or out-of-range input); outputs
        /// are untouched on false. Reuse proof lives on the definition.
        [[nodiscard]] bool buildColliderViewsDelta(std::span<const sim::ChunkPos> sorted_needed,
            std::span<const sim::ChunkPos> dirty, Array<sim::ChunkPos> &listed_out,
            Array<Array<u16> > &pool_out, Array<physics::ColliderChunkView> &views_out);

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
        /// Keeps the current spawn count, so a profiling load persists across
        /// regeneration.
        [[nodiscard]] std::error_code regenerate(u64 seed);

        /// Replaces the generated world with a spawn-count override (agent
        /// count; 0 fails closed to `kAgentCount`, above `kAgentCountMax`
        /// returns `value_too_large`). Snapshot/registration order untouched.
        [[nodiscard]] std::error_code regenerate(u64 seed, u32 agent_count);

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

        /// Accumulated per-stage wall-clock timings (observation only; never
        /// merged with the translator's submit-side table. Submit runs on the
        /// presentation boundary with its own lifetime, so both tables are
        /// reported side by side instead of summed).
        [[nodiscard]] const StageTimings& stageTimings() const noexcept;

        /// Clears the accumulated stage timings (same paths as clearToolState).
        void resetStageTimings() noexcept;

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

        /// Records a rectangular dig intent served by the `tool` step on the
        /// next committed tick, stepOne, or paused update; the latest intent
        /// wins. Never routes through demolish errands; UI/smoke call this only.
        [[nodiscard]] std::error_code requestDig(sim::GlobalCellPos min, u32 width, u32 height);

        /// Total dug cells cached from applied intents (panel-safe).
        [[nodiscard]] u32 digTotal() const noexcept;

        /// Chunks touched by the most recent edit, for the overlay (panel-safe).
        [[nodiscard]] std::span<const sim::ChunkPos> lastEditChunks() const noexcept;

        /// Pushes edit-touched chunks into presentation (`markChunkChanged`).
        /// Call after `update`/`stepOne` and before the translator submit.
        /// Drains the presentation set only; collider sets drain in
        /// stepPhysics, never here.
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
