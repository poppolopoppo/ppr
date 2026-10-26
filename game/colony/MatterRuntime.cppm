module;

export module game.colony.matter;

import engine.core;
import engine.sim;
import engine.physics;
import engine.matter;
import engine.app;
import engine.shader;
import engine.rhi;
import game.colony.content;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // durable game state and physical adapters
    // ------------------------------------------------------------------

    inline constexpr u32 kMatterGridEdge = sim::kWorldEdge + 7u;
    inline constexpr float kMatterOrigin = 3.0f;
    inline constexpr u32 kMatterTerrainEdge = sim::kWorldEdge + 1u;
    inline constexpr float kMatterGravityY = -9.8f;

    struct MatterIdentity {
        u64 m_world{};
        /// Durable publication count, independent of a live solver's resource epoch.
        u64 m_publication{};
        u64 m_terrain_revision{};

        [[nodiscard]] bool operator==(const MatterIdentity &) const noexcept = default;
    };

    /// Owned admission or exact demand-snapshot state. Ordinary resident ticks
    /// keep only immutable physical/ID/gas metadata in the driver; CPU coordinate
    /// queries are unavailable until an explicitly reconstructed current snapshot.
    /// Physical records match solver rows in stable-ID order; gas is unsimulated.
    struct MatterState {
        Array<matter::GpuParticle> m_particles{};
        Array<sim::MatterParticle> m_records{};
        Array<sim::MatterParticle> m_gas{};
        MatterIdentity m_identity{};
        u64 m_next_id{1u};
        double m_cell_metres{};
        double m_thickness_metres{};
        matter::GpuParameters m_profile{};
    };

    [[nodiscard]] Expected<MatterState> makeMatterState(
        const sim::MatterParticleSnapshot &particles, double cell_metres, double thickness_metres, float gravity_y,
        u64 world);
    [[nodiscard]] Expected<sim::MatterParticleSnapshot> captureMovingMatter(
        const MatterState &state, std::span<const matter::GpuParticle> particles);

    [[nodiscard]] std::error_code validateMatterState(
        const MatterState &state, std::span<const sim::GenerationMaterial> materials, double cell_metres,
        double thickness_metres, float gravity_y);
    [[nodiscard]] std::error_code validateMatterProfile(
        const matter::GpuParameters &profile, double cell_metres, double thickness_metres, float gravity_y);

    /// Pure candidate preparation: no IDs, grid cells or carried fields publish.
    /// Expected absence means new contour coverage deferred this whole cohort.
    [[nodiscard]] Expected<std::optional<MatterState>> prepareMatterTerrainEdit(
        const MatterState &source, const sim::ChunkGrid &original,
        std::span<const sim::ChunkGrid::CapturedChunk> candidate, const ContentCatalog &catalog);

    [[nodiscard]] Expected<Array<u32>> makeMatterTerrain(
        const sim::ChunkGrid &grid, std::span<const u16> solid_materials);
    [[nodiscard]] Expected<Array<matter::GpuBodyBoundary>> predictMatterBodies(
        std::span<const physics::BodyBoundarySnapshot> bodies, double cell_metres,
        const matter::GpuParameters &profile);

    // ------------------------------------------------------------------
    // lane-affine device owner
    // ------------------------------------------------------------------

    struct MatterReadback {
        Array<matter::GpuParticle> m_particles{};
        matter::GpuAccounting m_accounting{};
        MatterIdentity m_source{};
    };

    /// Correlates one request to one facade instance, including repeated polls.
    class MatterRequest {
        friend class MatterRuntime;

        std::weak_ptr<const u8> m_instance{};
        u64 m_generation{};
    };

    struct MatterCompletion {
        std::optional<MatterReadback> m_readback{};
        std::optional<GridParticleGpuView> m_view{};
        RenderLaneOperationCompletion m_execution{};
    };

    /// Application owns this facade. Every solver/RHI/shader operation, including
    /// release, runs on the injected device lane; nullptr denotes an inline owner.
    /// Requests/results own their storage. No GPU span crosses the lane boundary.
    /// Facade calls belong to main; lane operations never recursively call it.
    class MatterRuntime {
        struct State;
        struct Pending;
        std::shared_ptr<State> m_state{};
        std::shared_ptr<const u8> m_instance{};
        std::unique_ptr<Pending> m_pending{};
        RenderLane *m_lane{};
        u64 m_generation{};
        matter::GpuAccounting m_last_accounting{};
        matter::GpuFailure m_last_failure{};

        [[nodiscard]] Expected<MatterRequest> enqueue(
            RenderLaneOperation operation, std::shared_ptr<std::optional<MatterReadback>> readback = {},
            bool shutdown = false, bool discard = false);

    public:
        MatterRuntime();
        ~MatterRuntime();

        MatterRuntime(const MatterRuntime &) = delete;
        MatterRuntime &operator=(const MatterRuntime &) = delete;

        // Every begin owns its inputs once. Advance alone admits/takes the
        // existing lane slot; absence is transport pending, never GPU readiness.
        [[nodiscard]] Expected<MatterRequest> beginInitialize(
            RenderLane *lane, rhi::IDevice &device, IShaderService &shaders, std::filesystem::path content_root,
            const ITimerClock *observation_clock = nullptr);
        [[nodiscard]] Expected<MatterRequest> beginPrepareResidentWorld(const MatterState &state, Array<u32> terrain);
        [[nodiscard]] Expected<MatterRequest> beginPublishWorld(MatterIdentity identity);
        [[nodiscard]] Expected<MatterRequest> beginCancelWorld();
        [[nodiscard]] Expected<MatterRequest> beginResumeWorld(MatterIdentity identity);
        [[nodiscard]] Expected<MatterRequest> beginSubmitResident(
            MatterIdentity source, Array<matter::GpuBodyBoundary> predicted_bodies, u32 body_count);
        [[nodiscard]] Expected<MatterRequest> beginPoll(MatterIdentity source);
        [[nodiscard]] Expected<MatterRequest> beginPublish(MatterIdentity source);
        [[nodiscard]] Expected<MatterRequest> beginDiscard();

        /// Routes the solver's existing post-completion failure seam through its owner.
        [[nodiscard]] Expected<MatterRequest> beginInjectCollectionFailureForTest(std::error_code cause);

        // Explicit demand capture remains frozen through CPU reconstruction.
        [[nodiscard]] Expected<MatterRequest> beginSnapshot(MatterIdentity source);
        [[nodiscard]] Expected<MatterRequest> beginPollSnapshot(MatterIdentity source);
        [[nodiscard]] Expected<MatterRequest> beginEndSnapshot(MatterIdentity source);

        // Inventory edits share the active solver and retain its current rows.
        [[nodiscard]] Expected<MatterRequest> beginPrepareEdit(
            MatterIdentity source, const MatterState &candidate, u32 retained_count, Array<u32> terrain);
        [[nodiscard]] Expected<MatterRequest> beginPollEdit(MatterIdentity target);
        [[nodiscard]] Expected<MatterRequest> beginPublishEdit(MatterIdentity target);
        [[nodiscard]] Expected<MatterRequest> beginDrain();
        [[nodiscard]] Expected<MatterRequest> beginShutdown();

        [[nodiscard]] Expected<std::optional<MatterCompletion>> advance(const MatterRequest &request);
        [[nodiscard]] std::error_code cancel(const MatterRequest &request);
        [[nodiscard]] bool serviceReady() const noexcept;
        [[nodiscard]] bool requestPending() const noexcept;

        /// Copies of last acknowledged device state, including during pending work.
        [[nodiscard]] Expected<matter::GpuAccounting> accounting();
        [[nodiscard]] Expected<matter::GpuFailure> failure();
    };
}
