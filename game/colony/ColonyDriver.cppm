module;

export module game.colony.driver;

import engine.core;
import engine.physics;
import engine.sim;
import game.colony;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.translator;
import game.colony.worldgen;
import game.colony.content;

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
        /// Explicit publication identity for isolated candidates. Zero keeps
        /// the legacy in-place lifecycle's monotonically incremented identity.
        u64 m_world_generation{};
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
    public:
        /// Revision-tagged dirty-set row used for capture and ack.
        struct DirtyChunk {
            sim::ChunkPos m_pos{};
            u64 m_revision{};
        };

    private:
        struct PendingDig {
            sim::GlobalCellPos m_min{};
            u32 m_width{};
            u32 m_height{};
        };

        struct ErrandProgress {
            sim::Entity m_entity{};
            u32 m_original{};
            u32 m_candidate{};
        };
        struct EditCandidate {
            Array<sim::ChunkGrid::CapturedChunk> m_chunks{};
            Array<sim::MatterParticleBatch> m_particles{};
            Array<sim::ChunkPos> m_touched{};
            Array<DirtyChunk> m_manifest{};
            Array<ErrandProgress> m_errands{};
            u32 m_dug{};
            bool m_has_dig{};
            bool m_colliders_started{};
            bool m_colliders_ready{};
        };
        std::optional<EditCandidate> m_edit{};
        bool m_require_edit_publication{};
        bool m_resume_edit_tick{};
        bool m_single_step_pending{};
        u32 m_held_slices{};

        Colony m_colony{};
        std::optional<sim::FixedTimestep> m_timestep{};
        u64 m_seed{};
        // Genuine world generation: bumped on every successful init/
        // regenerate. The ack manifest rides this (not the user seed), so
        // a regenerated world — even one re-using the same seed value —
        // can never match a stale result's ack tags.
        u64 m_world_generation{0u};
        u64 m_tick_count{};
        u32 m_agent_count{kAgentCount};
        u32 m_tick_hz{60u};
        u32 m_speed{1u};
        bool m_paused{false};
        bool m_initial_matter_baseline_valid{true};
        std::optional<sim::Entity> m_controlled{};
        PlayerCommand m_player_command{};
        sim::Registry m_registry{};
        sim::StepRegistry m_steps{};
        Pathfinder m_finder{};
        Array<DirtyChunk> m_present_dirty{};
        // Last revision issued per chunk position, surviving row removal:
        // a chunk re-dirtied after its ack continues from this revision
        // instead of restarting at 1, so a stale in-flight ack can never
        // clear the new edit (revision ABA). Cleared only on worldgen.
        Array<DirtyChunk> m_present_revision_history{};
        Array<sim::ChunkPos> m_collider_dirty{};
        DigStats m_dig_stats{};
        // Bounded FIFO of accepted dig rects (drop-oldest past
        // kMaxPendingDigs); drained one rect per tool-system tick in
        // accept order. Transient intent only — never snapshotted.
        static constexpr std::size_t kMaxPendingDigs = 256u;
        Array<PendingDig> m_pending_digs{};
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
        Array<Array<u16>> m_cached_pool{};
        Array<physics::ColliderChunkView> m_cached_views{};
        PathCounts m_counts{};
        u32 m_errands{};
        physics::Scene m_scene{};
        physics::ChunkColliders m_colliders{};
        Array<AgentBody> m_bodies{};
        /// Slot-indexed row lookup for m_bodies (none_v = no body). Rebuilt
        /// wholesale by createAgentBodies/restoreBodies; extended in
        /// moveAgents when a new body row is appended. DERIVED state only:
        /// never snapshotted, never compared. Invariant: agents are never
        /// destroyed mid-run in this slice so rows are never removed; a
        /// future destroy path must rebuild the map.
        Array<u32> m_body_by_slot{};
        Array<AgentSummary> m_agents{};
        std::error_code m_step_error{};
        const char *m_step_stage{""};
        bool m_steps_bound{false};
        bool m_physics_ready{false};

    private:
        std::error_code initPhysics();

        [[nodiscard]] std::error_code initializeRuntime(
            const ColonyDriverDesc &desc, TimeSpan period, const sim::GenerationExecution *execution,
            const sim::Snapshot *restored = nullptr);

        void shutdownPhysics() noexcept;
        void collectColliderInterest(Array<sim::ChunkPos> &needed);
        [[nodiscard]] std::error_code prepareEdit(bool advance_build);
        [[nodiscard]] std::error_code prepareEditColliders();
        [[nodiscard]] std::error_code runHeldTicks();


        std::error_code spawnColonyAgents(const IContext *context = nullptr);

        std::error_code createAgentBodies(const IContext *context = nullptr);

        void refreshAgents();

        void moveAgents() noexcept;

        [[nodiscard]] std::error_code moveAgent(sim::Entity entity) noexcept;

        void stepPhysics() noexcept;

        void mirrorBodyStates() noexcept;

        std::error_code rebuildChunks(
            std::span<const sim::ChunkPos> dirty, std::span<const sim::ChunkPos> needed, u32 max,
            const IContext *context = nullptr);

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
        [[nodiscard]] Expected<bool> buildColliderViewsDelta(
            std::span<const sim::ChunkPos> sorted_needed, std::span<const sim::ChunkPos> dirty,
            Array<sim::ChunkPos> &listed_out, Array<Array<u16>> &pool_out,
            Array<physics::ColliderChunkView> &views_out);

        [[nodiscard]] std::error_code copyColliderChunk(sim::ChunkPos position, Array<u16> &occupancy) const;

    public:
        /// Explicit legacy smoke/many-agent fixture; production uses initPrepared.
        [[nodiscard]] std::error_code initLegacyFixture(
            const ColonyDriverDesc &desc, const sim::GenerationExecution *execution = nullptr);

        /// Adopts a complete generated owner at this driver's stable address.
        /// Prepared sessions start one controlled actor at the validated start.
        [[nodiscard]] std::error_code initPrepared(
            const ColonyDriverDesc &desc, PreparedWorld world, const sim::GenerationExecution *execution = nullptr);
        /// Initializes an unpublished checkpoint candidate without spawning actors.
        [[nodiscard]] std::error_code initRestored(
            const ColonyDriverDesc &desc, PreparedWorld world, const sim::Snapshot &snapshot,
            const sim::MatterParticleSnapshot &particles, u64 tick, bool initial_matter_valid,
            std::optional<sim::Entity> controlled, const sim::GenerationExecution *execution = nullptr);
        [[nodiscard]] Expected<sim::Snapshot> captureFullWithBodies(const IContext *context = nullptr) const;
        [[nodiscard]] u32 tickHz() const noexcept;
        [[nodiscard]] bool captureReady() const noexcept;
        [[nodiscard]] const PreparedWorld *preparedWorld() const noexcept;
        [[nodiscard]] bool initialMatterBaselineValid() const noexcept;
        [[nodiscard]] sim::MatterParticleSnapshot captureParticles() const;
        [[nodiscard]] std::span<const sim::MatterParticle> particleChunk(sim::ChunkPos position) const noexcept;
        [[nodiscard]] u32 particleCount() const noexcept;
        [[nodiscard]] std::span<const sim::MatterParticleBatch> editParticles() const noexcept;
        // Borrowed immutable catalog; absent for explicit legacy fixtures.
        [[nodiscard]] const ContentCatalog *contentCatalog() const noexcept;
        [[nodiscard]] u16 boundaryMaterial() const noexcept;
        [[nodiscard]] u32 cellsPerMetre() const noexcept;
        // Exactly 4096 column-centre heights. False denotes a legacy fixture
        // or closed world and leaves the caller's output untouched.
        [[nodiscard]] bool sampleSurfaceHeights(std::span<float> heights) const noexcept;
        [[nodiscard]] double referenceThicknessMetres() const noexcept;
        [[nodiscard]] std::string_view profileKey() const noexcept;

        /// Interactive owner enables matching render-publication settlement.
        /// Headless fixtures retain synchronous edit execution.
        void requireEditPublication() noexcept;
        [[nodiscard]] bool editPending() const noexcept;
        [[nodiscard]] bool editReadyForPresentation() const noexcept;
        [[nodiscard]] std::span<const sim::ChunkGrid::CapturedChunk> editChunks() const noexcept;
        [[nodiscard]] std::span<const DirtyChunk> editManifest() const noexcept;
        /// Matching complete GPU-consumption manifest commits the held edit.
        [[nodiscard]] std::error_code settleEdit(std::span<const DirtyChunk> manifest);

        [[nodiscard]] std::error_code shutdown();

        /// Advances at most five fixed ticks; excess whole slices are dropped.
        [[nodiscard]] std::error_code update(TimeSpan elapsed);

        void setPaused(bool paused) noexcept;

        void togglePaused() noexcept;

        [[nodiscard]] std::error_code assignControl(std::optional<sim::Entity> entity);
        [[nodiscard]] std::optional<sim::Entity> controlledEntity() const noexcept;
        [[nodiscard]] std::error_code setPlayerCommand(PlayerCommand command) noexcept;
        void clearPlayerCommand() noexcept;

        /// Only valid while initialized and paused; runs exactly one tick.
        [[nodiscard]] std::error_code stepOne() noexcept;

        /// Accepts only speeds 1, 2, or 3.
        [[nodiscard]] std::error_code setSpeed(u32 speed) noexcept;

        /// Replaces the generated world, discarding tick and time backlog.
        /// Keeps the current spawn count, so a profiling load persists across
        /// regeneration.
        [[nodiscard]] std::error_code regenerateLegacyFixture(u64 seed);

        /// Replaces the generated world with a spawn-count override (agent
        /// count; 0 fails closed to `kAgentCount`, above `kAgentCountMax`
        /// returns `value_too_large`). Snapshot/registration order untouched.
        [[nodiscard]] std::error_code regenerateLegacyFixture(u64 seed, u32 agent_count);

        [[nodiscard]] const sim::ChunkGrid &grid() const noexcept;

        /// Mutable grid for the explicit edit functions below; all edits
        /// must flow through them so presentation and search invalidation
        /// stay coherent.
        [[nodiscard]] sim::ChunkGrid &grid() noexcept;

        [[nodiscard]] u64 seed() const noexcept;

        /// Monotonic world generation: every successful init/regenerate
        /// advances it. Identity for ack matching — never the user seed.
        [[nodiscard]] u64 worldGeneration() const noexcept;

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

        /// Records a rectangular dig intent served by the `tool` step on the
        /// next committed tick, stepOne, or paused update; the latest intent
        /// wins. Never routes through demolish errands; UI/smoke call this only.
        [[nodiscard]] std::error_code requestDig(sim::GlobalCellPos min, u32 width, u32 height);

        /// Total dug cells cached from applied intents (panel-safe).
        [[nodiscard]] u32 digTotal() const noexcept;

        /// Chunks touched by the most recent edit, for the overlay (panel-safe).
        [[nodiscard]] std::span<const sim::ChunkPos> lastEditChunks() const noexcept;

        /// Complete pending presentation-dirty set, accumulated across every
        /// edit since the last acknowledgement (panel-safe). Each awaited
        /// row carries the edit revision at the time the entry was last
        /// bumped — so revision-aware ackPresentChunks can clear only the
        /// matching generation: a re-edit to the same chunk after capture
        /// (same chunk, newer revision) is distinct and stays pending.
        /// Capture reads this — not lastEditChunks, which reports only the
        /// most recent edit — so a multi-edit frame snapshots every touched
        /// chunk.
        [[nodiscard]] std::span<const DirtyChunk> presentEditsCapture() const noexcept;

        /// Pushes edit-touched chunks into presentation (`markChunkChanged`).
        /// Call after `update`/`stepOne` and before the translator submit.
        /// Drains the presentation set only; collider sets drain in
        /// stepPhysics, never here.
        [[nodiscard]] std::error_code presentEdits(ColonyTranslator &translator) noexcept;

        /// Removes exactly the listed chunks from the pending
        /// presentation-dirty set, matching the captured revision per
        /// chunk: a re-edit to the same chunk after capture is a different
        /// generation and keeps its current entry (never cleared by a
        /// stale ack). Consumption acknowledges only what it presented;
        /// dirt that arrived after capture stays pending for the next
        /// frame.
        void ackPresentChunks(std::span<const DirtyChunk> acked) noexcept;

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
