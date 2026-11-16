module;
#include "pP/Macros.h"

export module engine.matter:gpu;

import :gpu.types;
import std;
import engine.core;
import engine.shader;
import engine.rhi;

export namespace pP::matter {
    // ------------------------------------------------------------------
    // completion and publication records
    // ------------------------------------------------------------------

    PPR_DECLARE_LOG_CATEGORY(MatterGpu)

    struct GpuFailure {
        std::error_code m_cause{};
        u32 m_flags{};
        // Second protocol word: the first-error claim latch, so a decoded verdict
        // can be reported without re-deriving it from the other fields.
        u32 m_claimed{};
        u32 m_node{0xffff'ffffu};
        u32 m_field{0xffff'ffffu};
        u32 m_particle{0xffff'ffffu};
        u32 m_body{0xffff'ffffu};
    };

    // ------------------------------------------------------------------
    // working-batch poll verdict
    // ------------------------------------------------------------------

    /// A completed working batch is publishable; a rejected terrain-position
    /// certificate is the one recoverable verdict and must be discarded.
    enum class EGpuTickVerdict : u8 {
        pending,
        complete,
        rejected_terrain_position,
    };

    /// Truthy only for a publishable candidate, so existing readiness checks keep
    /// their meaning while a caller can still name the recoverable rejection.
    struct GpuTickOutcome {
        EGpuTickVerdict m_verdict{EGpuTickVerdict::pending};

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return m_verdict == EGpuTickVerdict::complete;
        }

        [[nodiscard]] constexpr bool pending() const noexcept {
            return m_verdict == EGpuTickVerdict::pending;
        }

        [[nodiscard]] constexpr bool rejectedTerrainPosition() const noexcept {
            return m_verdict == EGpuTickVerdict::rejected_terrain_position;
        }
    };

    struct GpuAccounting {
        u64 m_device_bytes{};
        u64 m_readback_capacity_bytes{};
        u64 m_last_readback_bytes{};
        u64 m_last_particle_upload_bytes{};
        u64 m_last_particle_copy_bytes{};
        u64 m_epoch{};
        u64 m_dispatches{};
        u32 m_particle_count{};
        u32 m_shape_count{};
        u32 m_body_count{};
        u32 m_substeps{};
    };

    /// Owned full state materialized only for an explicit frozen-epoch capture.
    struct GpuSnapshot {
        Array<GpuParticle> m_particles{};
        u64 m_epoch{};
    };

    /// Strong resource ownership plus a revocation value for unencoded readers.
    /// The caller must select this only after its dependent joint CPU commit.
    /// The pair is the split device row storage; a semantic record needs unpack().
    struct GpuCommittedView {
        rhi::ComPtr<rhi::IBuffer> m_hot{};
        rhi::ComPtr<rhi::IBuffer> m_cold{};
        std::shared_ptr<const std::atomic<u64>> m_live_epoch{};
        u64 m_epoch{};
        u32 m_count{};
    };

    // ------------------------------------------------------------------
    // sole owning GPU solver
    // ------------------------------------------------------------------

    /// Value-only inspection of opt-in diagnostics; no GPU resource escapes its owner.
    struct GpuTimingInspection {
        std::error_code m_error{};
        u64 m_completed_samples{};
        u64 m_epoch{}, m_signal{}, m_first_gpu_timestamp{}, m_last_gpu_timestamp{};
        u32 m_query_count{};
    };

    /// Inventory-only edits retain the exact committed terrain resources and
    /// constants. Preserve requires an empty TerrainView, avoiding two inputs.
    enum class ETerrainUpdate : u8 {
        replace,
        preserve,
    };

    /// Single-owner runtime. Device, queue and shader session are injected and
    /// must outlive shutdown. No renderer, sim, physics or service lookup occurs.
    /// A submitted working batch never replaces committed state automatically.
    class GpuSolver {
        struct State;
        std::unique_ptr<State> m_state;

        [[nodiscard]] std::error_code prepareReplacement_(
            std::span<const GpuParticle> particles, std::span<const GpuStaticShape> shapes, u64 expected_epoch,
            TerrainView terrain, bool append, ETerrainUpdate terrain_update, GpuMaterialTableView properties);

    public:
        GpuSolver() noexcept;
        ~GpuSolver();

        GpuSolver(GpuSolver &&) noexcept;
        GpuSolver &operator=(GpuSolver &&) = delete;

        GpuSolver(const GpuSolver &) = delete;
        GpuSolver &operator=(const GpuSolver &) = delete;

        /// Creates programs and bounded grid resources; starts with no particles.
        /// A nonnull live observation clock opts into bounded diagnostic queries;
        /// the injected clock must outlive shutdown. Null allocates no query pool.
        [[nodiscard]] std::error_code initialize(
            rhi::IDevice &device, rhi::ICommandQueue &queue, IShaderService &shaders,
            const std::filesystem::path &content_root, GpuParameters profile,
            const ITimerClock *observation_clock = nullptr);

        /// Prepare all replacement resources before external terrain settlement.
        /// This freezes step admission until publication or explicit cancellation.
        [[nodiscard]] std::error_code prepareReplacement(
            std::span<const GpuParticle> particles, std::span<const GpuStaticShape> shapes, u64 expected_epoch,
            TerrainView terrain = {}, ETerrainUpdate terrain_update = ETerrainUpdate::replace,
            GpuMaterialTableView properties = {});

        /// Frozen edit candidate: preserve old rows by GPU copy and upload only
        /// appended rows. pollReplacement must prove its copy before publication.
        [[nodiscard]] std::error_code prepareAppend(
            std::span<const GpuParticle> appended, u64 expected_epoch, TerrainView terrain = {},
            ETerrainUpdate terrain_update = ETerrainUpdate::replace, GpuMaterialTableView properties = {});
        [[nodiscard]] Expected<bool> pollReplacement();
        [[nodiscard]] std::error_code cancelPreparedReplacement();

        /// Single-owner commit after external settlement, with no allocations or
        /// GPU calls. Requires a successfully prepared, still-frozen replacement.
        void publishReplacement() noexcept;
        void cancelReplacement() noexcept;

        /// One working batch; game calls with eight substeps and the world's saved
        /// schedule (new worlds use kDefaultIterations). The API default stays K2
        /// for source fixtures. The admitted iteration counts are {2, 5, 10}; every other
        /// count fails before GPU work.
        /// Focused fixtures may use fewer complete substeps.
        /// One-way body inputs, GPU publication checks and compact status only.
        /// No full particle copy, CPU candidate construction or reaction readback.
        [[nodiscard]] std::error_code submitResidentTick(
            u64 expected_epoch, u32 substeps = kGameSubsteps, u32 iterations = kSourceIterations,
            std::span<const GpuBodyBoundary> predicted_boundaries = {}, u32 body_count = 0u, GpuRadialForce force = {});

        /// Freezes mutation at this exact committed epoch until pollSnapshot takes
        /// the owned bytes. Cancellation/teardown must drain an admitted copy.
        [[nodiscard]] std::error_code beginSnapshot(u64 expected_epoch);
        [[nodiscard]] Expected<std::optional<GpuSnapshot>> pollSnapshot();
        [[nodiscard]] std::error_code cancelSnapshot();

        /// Pending means the fence has not completed. A complete candidate is
        /// publishable. A terrain-position certificate rejection is a bounded
        /// verdict: the caller discards the candidate and the committed epoch
        /// stays published. Every other rejected verdict remains a fatal error.
        [[nodiscard]] Expected<GpuTickOutcome> pollTick();

        /// External dependent simulation epoch validation precedes this commit.
        void publishTick() noexcept;
        [[nodiscard]] std::error_code discardTick();

        /// Current GPU hot/cold row pair; full CPU rows need an owned snapshot.
        [[nodiscard]] Expected<GpuCommittedView> committedView() const;
        [[nodiscard]] GpuParameters profile() const noexcept;
        [[nodiscard]] GpuAccounting accounting() const noexcept;
        [[nodiscard]] const GpuFailure &failure() const noexcept;
        [[nodiscard]] bool busy() const noexcept;

        /// Focused operational fault seam: fail collection after completion proof.
        void injectCollectionFailureForTest(std::error_code cause) noexcept;
        /// Focused verdict seam: replace the collected six error words so a
        /// synthetic rejection can be classified without undocumented state.
        void injectTickVerdictForTest(std::optional<std::array<u32, 6u>> words) noexcept;
        [[nodiscard]] GpuTimingInspection inspectTimingForTest() const noexcept;

        /// Freeze/capture and teardown use this explicit completion boundary.
        /// An unsuccessful wait retains every possibly in-flight resource.
        [[nodiscard]] std::error_code drain();
        [[nodiscard]] std::error_code shutdown();
    };
}
