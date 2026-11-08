module;

export module engine.app:renderer.lighting_pass;

import :renderer.lighting;
import :renderer.optical_geometry;
import :renderer.frame_fence;
import :renderer.types;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // explicit optical storage qualification owner
    // ------------------------------------------------------------------

    enum class GpuIntervalStorage : u8 {
        fp32,
        rgb16f
    };

    // Immutable per pass after initialize. The experimental option is admitted
    // only on Vulkan+Double, with caller-checked RTE64 evidence for that exact
    // device/session/configuration. This is not runtime capability discovery.
    enum class TerrainPrecision : u8 {
        strict_fp32_qualification,
        qualified_fp64_refinement_experimental
    };

    struct TerrainPrecisionSelection {
        TerrainPrecision m_precision{TerrainPrecision::strict_fp32_qualification};
        bool m_rte64_qualified_for_device_session{};
    };

    struct TerrainPrecisionStatistics {
        u64 m_query_upper_bound{};
        u32 m_attempted{}, m_qualified{}, m_unresolved{}, m_bound_error{};
    };

    struct GpuTraceSnapshot {
        std::vector<RadianceInterval> m_intervals{};
        // Selected cell/block spans; RC records only this slot's local connector
        // spans, excluding ancestor traversal. Status remains per stored interval.
        std::vector<u32> m_segments{};
        // Qualification-only bit validation from existing readback, including C1.
        // Empty for FP32; no additional GPU storage or readback allocation.
        std::vector<std::array<u32, 4>> m_packed_words{};
        std::optional<RadianceInterval> m_c1_probe0_bin0{};
    };

    // ------------------------------------------------------------------
    // immutable completed terrain receiver generation
    // ------------------------------------------------------------------

    // These are the existing Grid world IDs plus the optical attachment revision.
    // The consumer compares them to its actual resident publication, not an ack.
    struct TerrainReceiverCohort {
        u64 m_job_id{}, m_world_generation{}, m_content_fingerprint{}, m_geometry_revision{};

        [[nodiscard]] bool operator==(const TerrainReceiverCohort &) const noexcept = default;
    };

    struct TerrainReceiverResult {
        float m_mean[3]{};
        u32 m_status{};
    };

    enum class TerrainBackgroundOutput : u8 {
        disabled,
        c0_mean
    };

    struct TerrainBackgroundSource;

    enum class TerrainReceiverOutput : u8 {
        mean_only,
        angular16
    };

    struct TerrainReceiverAngular {
        float m_radiance[3]{};
        u32 m_status{};
    };

    // The same shape/domain predicate gates completed angular publication.
    [[nodiscard]] std::error_code validateTerrainReceiverAngular(
        std::span<const TerrainReceiverAngular> rows, u32 receiver_count);

    class TerrainReceiverGpuGeneration {
    public:
        [[nodiscard]] const std::shared_ptr<const TerrainReceiverSet> &receivers() const noexcept;
        [[nodiscard]] TerrainReceiverCohort cohort() const noexcept;
        [[nodiscard]] rhi::IBuffer *featuresBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *resultsBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *cellsBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *componentsBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *candidatesBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *c1Buffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *angularBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *backgroundMeanBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *directionsBuffer() const noexcept;
        [[nodiscard]] u32 angularDirectionOffset() const noexcept;
        [[nodiscard]] u32 angularDirectionCount() const noexcept;
        [[nodiscard]] u64 bufferBytes() const noexcept;
        [[nodiscard]] u64 largestBufferBytes() const noexcept;
        [[nodiscard]] bool matchesDevice(const rhi::IDevice &device) const noexcept;
        [[nodiscard]] const TerrainPrecisionStatistics *precisionStatistics() const noexcept;

    private:
        friend class RadianceCascadePass;

        std::shared_ptr<const TerrainReceiverSet> m_receivers{};
        std::shared_ptr<TerrainBackgroundSource> m_background{};
        std::unique_ptr<const TerrainPrecisionStatistics> m_precision_statistics{};
        rhi::ComPtr<rhi::IBuffer> m_features{}, m_results{}, m_cells{}, m_components{}, m_candidates{}, m_c1{},
            m_directions{}, m_angular{};
        rhi::IDevice *m_device{};
        TerrainReceiverCohort m_cohort{};
        u64 m_bytes{}, m_largest{};
        u32 m_angular_direction_offset{}, m_angular_direction_count{};
    };

    // ------------------------------------------------------------------
    // immutable completed exact-view cavern backwall lighting
    // ------------------------------------------------------------------

    struct TerrainBackgroundStatistics {
        u64 m_query_upper_bound{};
        u32 m_attempted{}, m_qualified{}, m_unresolved{}, m_bound_error{};
        u32 m_outside_plane{}, m_samples{}, m_invalid_values{};
    };

    [[nodiscard]] std::error_code validateTerrainBackgroundCompletion(
        const TerrainBackgroundStatistics &statistics, u32 width, u32 height);

    class TerrainBackgroundGpuView {
    public:
        [[nodiscard]] const std::shared_ptr<const TerrainReceiverGpuGeneration> &source() const noexcept;
        [[nodiscard]] rhi::IBuffer *resultsBuffer() const noexcept;
        [[nodiscard]] const TerrainBackgroundStatistics &statistics() const noexcept;
        [[nodiscard]] bool matches(
            const CameraSnapshot &camera, int2 extent, const rhi::IDevice &device) const noexcept;
        [[nodiscard]] u64 bufferBytes() const noexcept;
        [[nodiscard]] u64 largestBufferBytes() const noexcept;

    private:
        friend class RadianceCascadePass;

        std::shared_ptr<const TerrainReceiverGpuGeneration> m_source{};
        rhi::ComPtr<rhi::IBuffer> m_results{};
        CameraSnapshot m_camera{};
        TerrainBackgroundStatistics m_statistics{};
        int2 m_extent{};
        u64 m_bytes{};
    };

    // Single prepared batch, with explicit caller-owned submission/fence control.
    // Cell/terrain numerical qualification and the bounded real-scene receiver
    // producer share this owner. Full-world scheduling remains unsupported.
    // Device and shader service outlive this owner AND every acquired generation.
    // Consumers hold strong leases through accepted draw completion; unknown
    // submissions retain resources until explicit quiescence/removal proof.
    class RadianceCascadePass {
    public:
        // Initial complete-fixture qualification only. This is not the numeric
        // estimator or future streamed solver's world-size/quality limit.
        static constexpr u32 kMaxCompleteFixtureCells{4096u};

        RadianceCascadePass() noexcept;
        ~RadianceCascadePass();

        RadianceCascadePass(const RadianceCascadePass &) = delete;
        RadianceCascadePass &operator=(const RadianceCascadePass &) = delete;

        [[nodiscard]] std::error_code initialize(
            rhi::IDevice &device, IShaderService &shaders, const std::filesystem::path &content_root,
            TerrainPrecisionSelection precision = {});

        // World-origin subtraction occurs in double; GPU origins are local fine
        // cells. All coefficients remain per metre. Reserve covers non-buffer
        // allocations/headroom; demand includes candidate+previous live buffers,
        // including a GPU-built vacuum hierarchy over the captured optical rows.
        [[nodiscard]] std::error_code prepareTrace(
            const OpticalGridView &grid, std::span<const OpticalRay> rays, u64 budget_bytes, u64 reserve_bytes,
            u64 max_resource_bytes, FenceCompletionValue completed,
            GpuIntervalStorage storage = GpuIntervalStorage::fp32);
        [[nodiscard]] std::error_code encodeTrace(const PreRenderContext &context);

        // Explicit storage qualification; transport/merge arithmetic stays FP32.
        // rgb16f stores independent RGB L/T halves plus status in 16 bytes;
        // local connector spans occupy a separate bounded diagnostic plane.
        // No automatic fallback is performed. Request stride must match selection.
        // One source generation, no published C1 or
        // gameplay scalar allocation yet. Adds known direction/readback/previous
        // buffers to the caller's additional reserve before RC admission.
        [[nodiscard]] std::error_code prepareCascades(
            const OpticalGridView &grid, const RcEstimateRequest &request, FenceCompletionValue completed,
            GpuIntervalStorage storage = GpuIntervalStorage::fp32);
        [[nodiscard]] std::error_code encodeCascades(const PreRenderContext &context);

        // Noise-free rounded opaque/vacuum geometry qualification only. Uses
        // the existing FP32 protocol and lifecycle; packed terrain is unsupported.
        // Numeric ambiguity is visible as argument_out_of_domain at readback.
        // After input/budget/fence checks, first preparation creates only its
        // selected terrain pipelines; successful pipelines survive later retries.
        [[nodiscard]] std::error_code prepareTerrainTrace(
            const OpticalTerrainSnapshot &terrain, std::span<const OpticalRay> rays, u64 budget_bytes,
            u64 reserve_bytes, u64 max_resource_bytes, FenceCompletionValue completed);
        [[nodiscard]] std::error_code prepareTerrainCascades(
            const OpticalTerrainSnapshot &terrain, const RcEstimateRequest &request, FenceCompletionValue completed);

        // Bounded real-scene producer. Keeps the physical witness separate from
        // visual FP32 centres; completes C1 then receiver reconstruction without
        // a full C0 directional output. Every externally held generation counts
        // toward admission. Device/services must outlive all acquired leases.
        // Angular output is opt-in: sixteen spatially composed RGB/status rows
        // per receiver, separately from the unchanged mean row. All bins are
        // validated after completion before a generation may be published.
        // Optional background output executes C0 in existing parity scratch and
        // retains its8-direction mean plus the same immutable query resources.
        [[nodiscard]] std::error_code prepareTerrainReceivers(
            std::shared_ptr<const TerrainReceiverSet> receivers, TerrainReceiverCohort cohort,
            const RcEstimateRequest &request, FenceCompletionValue completed, bool qualification_readback = false,
            TerrainReceiverOutput output = TerrainReceiverOutput::mean_only,
            TerrainBackgroundOutput background = TerrainBackgroundOutput::disabled);
        [[nodiscard]] std::expected<std::vector<TerrainReceiverResult>, std::error_code> readTerrainReceivers(
            FenceCompletionValue completed);
        [[nodiscard]] std::expected<std::vector<TerrainReceiverAngular>, std::error_code> readTerrainReceiverAngular(
            FenceCompletionValue completed);
        [[nodiscard]] std::expected<std::shared_ptr<const TerrainReceiverGpuGeneration>, std::error_code>
        acquireTerrainReceiverView(FenceCompletionValue completed);

        // This bounded phase admits axis-aligned orthographic +Z camera rays
        // with affine W=1 and the receiving plane inside the near/far segment.
        // Tilted, parallel, backward and perspective views reject explicitly.
        // Five exact raster positions per pixel: cached1x centre then standard
        // indexedMSAA4. No fallible geometry work is deferred to a fragment.
        // The finite receiving plane is the source C0 convex hull; outside is void.
        [[nodiscard]] std::error_code prepareTerrainBackgroundView(
            std::shared_ptr<const TerrainReceiverGpuGeneration> source, const CameraSnapshot &camera, int2 extent,
            u64 budget_bytes, u64 reserve_bytes, u64 max_resource_bytes, FenceCompletionValue completed);
        [[nodiscard]] std::error_code encodeTerrainBackgroundView(const PreRenderContext &context);
        [[nodiscard]] std::expected<std::shared_ptr<const TerrainBackgroundGpuView>, std::error_code>
        acquireTerrainBackgroundView(FenceCompletionValue completed);

        // Empty for strict/cell/unsubmitted batches. Submitted incomplete fences
        // return try_again; unknown submissions and malformed counts are errors.
        // Failure counts remain observable; publication/readCompleted separately
        // reject unresolved/bound failures before exposing successful transport.
        [[nodiscard]] std::expected<std::optional<TerrainPrecisionStatistics>, std::error_code>
        readTerrainPrecisionStatistics(FenceCompletionValue completed);

        // Qualification-only readiness in trace/top/merge order. No compilation
        // or GPU work; all false before initialization and after shutdown.
        [[nodiscard]] std::array<bool, 3> terrainPipelineReadinessForQualification() const noexcept;

        // Explicit qualification observation of the SAME linked components
        // passed to RHI. Vulkan only; never writes during normal initialization.
        // receiver_only selects the new producer entry; false preserves the
        // original trace/top/merge observation without unused receiver codegen.
        [[nodiscard]] std::error_code writeTerrainSpirvForQualification(
            const std::filesystem::path &directory, bool receiver_only = false);

        // Explicit diagnostic capture of every executed cascade before parity
        // reuse. Requires preparation opt-in and a completed accepted fence;
        // exports failed raw records without validating or publishing them.
        [[nodiscard]] std::error_code writeTerrainCascadeReadbackForQualification(
            const std::filesystem::path &directory, FenceCompletionValue completed);

        // Exactly one disposition after encoding. Discard requires the caller to
        // destroy/abandon the unsubmitted command buffer first. An ambiguous submit
        // failure quarantines resources until explicit idle/removal proof.
        [[nodiscard]] std::error_code notifySubmitted(FenceCompletionValue value);
        [[nodiscard]] std::error_code discardUnsubmitted();
        [[nodiscard]] std::error_code notifySubmitFailed();

        [[nodiscard]] std::expected<GpuTraceSnapshot, std::error_code> readCompleted(FenceCompletionValue completed);
        [[nodiscard]] std::error_code shutdown(FenceCompletionValue completed);
        [[nodiscard]] std::error_code shutdown(const GpuQuiescenceResult &proof);

    private:
        [[nodiscard]] std::error_code prepareTrace_(
            const OpticalGridView &grid, std::span<const OpticalRay> rays, u64 budget_bytes, u64 reserve_bytes,
            u64 max_resource_bytes, FenceCompletionValue completed, GpuIntervalStorage storage,
            const OpticalTerrainSnapshot *terrain);
        [[nodiscard]] std::error_code prepareCascades_(
            const OpticalGridView &grid, const RcEstimateRequest &request, FenceCompletionValue completed,
            GpuIntervalStorage storage, const OpticalTerrainSnapshot *terrain,
            std::shared_ptr<const TerrainReceiverSet> receivers = {}, TerrainReceiverCohort cohort = {},
            bool qualification_readback = false, TerrainReceiverOutput output = TerrainReceiverOutput::mean_only,
            TerrainBackgroundOutput background = TerrainBackgroundOutput::disabled);

        struct State;
        std::unique_ptr<State> m_state{};
    };
}
