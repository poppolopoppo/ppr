module;
export module engine.app:renderer.world_radiance;

import :renderer.types;
import :renderer.frame_fence;
import :scene.camera;
import :renderer.lighting;
import :renderer.lighting_pass; // Existing four-word publication cohort only.
import engine.core;
import engine.rhi;
import engine.shader;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // immutable native-cell optical input
    // ------------------------------------------------------------------

    // A projection of existing selected-ID optics, not an authoring catalog.
    // Homogeneous cells retain canonical sigma/j/S; opaque S is used once.
    // Scattering and unavailable used rows remain unsupported.
    struct WorldRadianceMaterial {
        OpticalCell m_optics{};
        u32 m_material_id{};
        std::array<float, 3u> m_diffuse_albedo{}; // Authored linear tint * filtered mean * (1-metallic).
    };

    // Sparse, already volume-aggregated native cells. Sorted unique cell indices;
    // sigma/j are per metre and retain overfill. Terrain opaque has priority.
    struct WorldRadianceOverlayCell {
        u32 m_cell{};
        float m_coverage{};
        std::array<float, 3u> m_source{}, m_albedo{}, m_sigma{}, m_emission{};
    };

    // Field order is the existing positional capture contract; appended identity
    // and palette members retain that contract and natural alignment.
    struct WorldRadianceInput {
        std::span<const u16> m_material_ids{};
        std::span<const WorldRadianceMaterial> m_materials{};
        std::array<double, 2> m_min_m{};
        double m_cell_metres{};
        u32 m_width{}, m_height{};
        OpticalBoundary m_exterior{OpticalBoundary::unavailable};
        OpticalRgb m_sky{}; // Explicit incoming boundary radiance, default black.
        std::span<const WorldRadianceOverlayCell> m_overlay{};
        // Original IDs, including particle-only rows filtered from terrain IDs.
        std::span<const WorldRadianceMaterial> m_particle_materials{};
        GridWorldIdentity m_world_identity{};
        u64 m_terrain_revision{}; // Actual Matter terrain counter, not outer lighting revision.
    };

    struct WorldRadianceMatterIdentity {
        GridParticlePresentationIdentity m_particles{};
        u64 m_device_epoch{};

        [[nodiscard]] bool operator==(const WorldRadianceMatterIdentity &) const noexcept = default;
    };

    // Aggregate composed per-cell volume-emission admission over actual
    // projected participating cells. Records count and per-channel maxima; the
    // bound is not a whole-scene proof.
    struct WorldRadianceEmissionAggregate {
        u64 m_cells{};
        std::array<double, 3> m_maximum{};
        std::array<double, 3> m_bound{};
    };

    [[nodiscard]] Expected<WorldRadianceEmissionAggregate> admitWorldRadianceEmission(
        u64 cell_count, std::array<double, 3> per_cell_maximum);

    // Applies a finite positive display pre-exposure scalar to the observed
    // aggregate emission bound and rejects an unrepresentable or underflowed
    // exposed bound in the FP32 display domain. No clamping.
    [[nodiscard]] Expected<std::array<double, 3>> admitWorldRadianceExposure(
        const WorldRadianceEmissionAggregate &aggregate, double exposure_multiplier);

    struct WorldRadianceMatterStatistics {
        u64 m_input_rows{}, m_participating{}, m_vacuum{}, m_inactive{};
        u64 m_invalid{}, m_outside{}, m_unsupported{}, m_overlay_cells{};
        u64 m_partial_cells{}, m_hard_cells{}, m_changed_cells{};
        std::array<double, 3> m_emission_maximum{};
        std::array<double, 3> m_emission_bound{};
    };

    class WorldRadianceSource {
    public:
        static constexpr u32 kMaxNativeWorldEdge = 4096u;

        [[nodiscard]] static Expected<std::shared_ptr<const WorldRadianceSource>> capture(WorldRadianceInput input);

        // Caller pairs the canonical source with its actual resident world. This
        // small binding uses a real planned/committed Matter terrain revision.
        [[nodiscard]] static Expected<std::shared_ptr<const WorldRadianceSource>> bindMatterIdentity(
            std::shared_ptr<const WorldRadianceSource> terrain, GridWorldIdentity world, u64 terrain_revision);
        ~WorldRadianceSource();

        [[nodiscard]] u32 width() const noexcept;
        [[nodiscard]] u32 height() const noexcept;
        [[nodiscard]] double cellMetres() const noexcept;
        [[nodiscard]] std::array<double, 2> minimumMetres() const noexcept;
        [[nodiscard]] std::span<const u16> materialIds() const noexcept;
        [[nodiscard]] std::span<const u32> particleMaterialIds() const noexcept;
        [[nodiscard]] bool matchesTerrainSource(const WorldRadianceSource &other) const noexcept;
        [[nodiscard]] bool matchesProjectedOptics(const WorldRadianceSource &previous) const noexcept;
        [[nodiscard]] u64 retainedCpuBytes() const noexcept;
        [[nodiscard]] std::array<double, 3> underflowLossBound() const noexcept;
        [[nodiscard]] std::optional<WorldRadianceMatterIdentity> matterIdentity() const noexcept;
        [[nodiscard]] std::optional<WorldRadianceMatterStatistics> matterStatistics() const noexcept;
        [[nodiscard]] std::optional<WorldRadianceMatterIdentity> matterComparedIdentity() const noexcept;
        [[nodiscard]] FenceCompletionValue matterCompletionFence() const noexcept;

        // GPU-projected immutable rows only; retain this source through the
        // accepted reader fence. A committed-empty projection owns safe dummies.
        [[nodiscard]] rhi::IBuffer *overlayBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *overlayLookupBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *overlayAncestorBuffer() const noexcept;
        [[nodiscard]] u32 overlayCount() const noexcept;
        [[nodiscard]] u32 overlayLookupCapacity() const noexcept;
        [[nodiscard]] u32 overlayAncestorWords() const noexcept;

        // Compare the selected Grid row to the same canonical uploaded optics.
        [[nodiscard]] std::error_code validateMaterialProjection(u32 id, const OpticalCell &optics) const;
        [[nodiscard]] bool matchesMaterialDiffuseProjection(u32 id, std::array<float, 3u> albedo) const noexcept;

    private:
        friend class WorldRadiancePass;
        friend class WorldRadianceGameplayQuery;
        struct Data;
        explicit WorldRadianceSource(std::unique_ptr<const Data> data);
        std::unique_ptr<const Data> m_data;
    };

    // ------------------------------------------------------------------
    // renderer-independent local gameplay illumination query
    // ------------------------------------------------------------------

    struct WorldRadianceGameplayQuerySettings {
        std::array<i32, 2> m_minimum{};
        std::array<u32, 2> m_counts{};
    };

    struct WorldRadianceGameplayQueryResult {
        OpticalRgb m_radiance{};
        OpticalRgb m_transmittance{};
        u64 m_segments{};
    };

    // Captures one immutable CPU terrain window in global cell order. A
    // matter-bound source resolves to its native terrain rows; projected
    // particle rows are GPU-only and are not part of this local query. An
    // opaque base returns before overlay lookup, so its overlay row is ignored
    // and the cell stays opaque. Otherwise overlay cells with 0<c<1 are
    // rejected with not_supported because OpticalCell cannot represent their
    // coverage-weighted surface or residual transmission; full and zero
    // coverage follow worldCell. The window is a bounded local domain, not a
    // full-world certificate.
    //
    // trace() returns medium transmittance to the window boundary. The exterior
    // is vacuum when any sky channel is nonzero: a zero channel contributes no
    // sky and escaped transmittance is the medium transmittance to the boundary,
    // while each nonzero channel adds transmittance*sky once. An all-zero sky
    // makes the exterior opaque black (escaped transmittance is zero).
    class WorldRadianceGameplayQuery {
    public:
        static constexpr u32 kMaxWindowEdge = 256u;

        [[nodiscard]] static Expected<WorldRadianceGameplayQuery> capture(
            const WorldRadianceSource &source, WorldRadianceGameplayQuerySettings settings);
        // Clamped square window around one world-space point, in source cell
        // coordinates. Edge is clamped to [1, min(kMaxWindowEdge, source
        // extent)]; only edge == 0 is an argument error. Nonfinite centres and
        // nonpositive/nonfinite cell size are rejected rather than clamped.
        [[nodiscard]] static Expected<WorldRadianceGameplayQuerySettings> windowForWorldPoint(
            const WorldRadianceSource &source, std::array<double, 2> center_m, u32 edge);
        [[nodiscard]] Expected<WorldRadianceGameplayQueryResult> trace(const OpticalRay &ray) const;

        [[nodiscard]] u32 width() const noexcept;
        [[nodiscard]] u32 height() const noexcept;
        [[nodiscard]] double cellMetres() const noexcept;
        [[nodiscard]] std::array<double, 2> minimumMetres() const noexcept;
        [[nodiscard]] u64 retainedCpuBytes() const noexcept;

    private:
        WorldRadianceGameplayQuery(
            OpticalHierarchy hierarchy, OpticalRgb sky, std::array<double, 2> minimum, double cell_metres, u32 width,
            u32 height, bool vacuum_boundary, u64 retained_bytes);

        OpticalHierarchy m_hierarchy;
        OpticalRgb m_sky{};
        std::array<double, 2> m_minimum{};
        double m_cell_metres{};
        u64 m_retained_bytes{};
        u32 m_width{}, m_height{};
        bool m_vacuum_boundary{};
    };

    // ------------------------------------------------------------------
    // signed circular-harmonic field and explicit resource admission
    // ------------------------------------------------------------------

    // a0=mean(L), then a1,b1,...,a4,b4 in the normalized real Fourier basis.
    // AC uses v22 Streamer's geometry DC correction and sinc(n*pi/5) window.
    // Signed FP32 is the diagnostic control; no unsigned/shared-exponent pack.
    struct WorldRadianceCoefficients {
        float m_coefficients[27]{};
        u32 m_status{};
    };

    // Scalar GPU protocol: two-word identities avoid requiring shader Int64 operations.
    struct WorldRadianceProbeState {
        std::array<u32, 2> m_source_version{}, m_sample_time_ns{};
        float m_variation{};
        u32 m_samples{};
    };

    struct WorldRadianceStatistics {
        u64 m_query_upper_bound{}, m_expected_rows{};
        u64 m_queries_attempted{}, m_queries_completed{}, m_queries_failed{}, m_rows_written{};
        u64 m_invalid_rows{}, m_segments{}, m_skipped_blocks{};
        u64 m_rows_copied{}; // Included in rows_written; no new rays or copied donor query counts.
        u32 m_status{};
    };

    enum class WorldRadianceRefinedSpacing : u32 {
        native_cell_centres = 1u, // Explicit centre-aligned control, not literal v22.
        two_cell_centres = 2u
    };

    // The straight mode is a bounded small-scene reference, not scalable RC.
    enum class WorldRadianceTransport : u32 {
        cascade_diagnostic = 0u,
        straight256_reference = 1u,
        direct_window = 2u,
        temporal_cascades = 3u
    };

    // Integer receiver rectangle, independent of the full immutable source domain.
    // Native-cell units unless an API explicitly specifies probe-space coverage.
    // Direct windows are page-aligned; final world-edge pages may be partial.
    struct WorldRadianceReceiverWindow {
        std::array<i32, 2> m_minimum{};
        std::array<u32, 2> m_counts{};

        [[nodiscard]] bool operator==(const WorldRadianceReceiverWindow &) const noexcept = default;
        [[nodiscard]] bool contains(WorldRadianceReceiverWindow other) const noexcept;
    };

    struct WorldRadianceGameplayDemand {
        u64 m_sequence{};
        WorldRadianceReceiverWindow m_window{};
    };

    // A value-owned region of the certified field. Gameplay can sample it
    // after the render device, projection source and presentation leases retire.
    struct WorldRadianceGameplayField {
        TerrainReceiverCohort m_cohort{};
        std::optional<WorldRadianceMatterIdentity> m_matter{};
        std::vector<WorldRadianceCoefficients> m_rows{};
        std::array<double, 2> m_origin_metres{};
        double m_cell_metres{};
        u64 m_transport_version{};
        FenceCompletionValue m_completion{kFenceValueNone};
        WorldRadianceGameplayDemand m_demand{};
        WorldRadianceReceiverWindow m_probes{};
        u32 m_spacing{};
        i32 m_sky_hour{-1};

        [[nodiscard]] Expected<WorldRadianceCoefficients> sample(std::array<double, 2> world_metres) const;
    };

    /// Complete visible plane plus two native cells, unioned with explicit
    /// gameplay receiver demands, then aligned to32-cell pages. No source crop.
    [[nodiscard]] Expected<WorldRadianceReceiverWindow> planWorldRadianceReceiverWindow(
        const WorldRadianceSource &source, const CameraSnapshot &camera,
        std::span<const WorldRadianceReceiverWindow> additional = {});

    // ------------------------------------------------------------------
    // full-world temporal cascade quality and capacity
    // ------------------------------------------------------------------

    struct WorldRadianceTemporalSettings {
        u32 m_refined_spacing{4u};
        u32 m_cascade_count{6u};
        u32 m_base_directions{4u};
        float m_interval_scale{1.0f};
        i32 m_sky_hour{-1}; // -1 uses the source boundary. Otherwise sample the rotating daylight sky.

        [[nodiscard]] bool operator==(const WorldRadianceTemporalSettings &) const noexcept = default;
    };

    struct WorldRadianceTemporalLevel {
        u64 m_records{}, m_first_record{};
        std::array<i32, 2> m_minimum{};
        std::array<u32, 2> m_counts{};
        u32 m_spacing{}, m_directions{};
        float m_near_cells{}, m_far_cells{};
    };

    struct WorldRadianceTemporalProfile {
        std::array<WorldRadianceTemporalLevel, 8> m_levels{};
        u64 m_cascade_records{}, m_cascade_bytes{}, m_refined_bytes{}, m_probe_state_bytes{}, m_field_pool_bytes{};
        std::array<u32, 2> m_refined_counts{};
    };

    // Allocation-free quality admission. Source, upload and retired owners are
    // accounted by the pass in addition to these actual field-storage bytes.
    [[nodiscard]] Expected<WorldRadianceTemporalProfile> planWorldRadianceTemporalProfile(
        u32 width, u32 height, WorldRadianceTemporalSettings settings);

    // ------------------------------------------------------------------
    // producer settings and immutable publication
    // ------------------------------------------------------------------

    // Straight256 requires native_cell_centres and phase .5. It publishes
    // width*height rows (min={0,0}); cascade_count has no work in that mode.
    struct WorldRadianceSettings {
        u64 m_budget_bytes{}, m_reserve_bytes{}, m_max_resource_bytes{};
        u32 m_cascade_count{4u}; // C1..C3 for the64x36 initial scene.
        float m_phase{0.5f}; // One known phase across EVERY level/probe this generation.
        bool m_diagnostic_coefficients{}; // Explicit bounded readback, counted before allocation.
        bool m_measure_gpu{}; // Optional batch timestamps; no query allocation or writes when disabled.
        WorldRadianceRefinedSpacing m_refined_spacing{WorldRadianceRefinedSpacing::two_cell_centres};
        WorldRadianceTransport m_transport{WorldRadianceTransport::cascade_diagnostic};

        // Straight-reference publication can qualify a narrower presentation
        // format. The default preserves the existing FP32 producer domain;
        // a narrower ceiling is supported only by straight256_reference.
        float m_maximum_response{(std::numeric_limits<float>::max)()};
        // Native-cell demand rectangle. Temporal mode accepts an all-zero window for
        // the full field; otherwise it refreshes its complete probe/parent support.
        WorldRadianceReceiverWindow m_receiver_window{};
        // Direct-window scheduling only: multiples of32 in [32,1024].
        u32 m_receiver_rows_per_quantum{256u};
        WorldRadianceTemporalSettings m_temporal{};
        u32 m_temporal_query_budget{131072u}; // Conservative ray-work ceiling per accepted batch.
        float m_temporal_history_weight{0.5f}; // Previous-sample retention per 1/60 second of injected time.
        // Producer allowance after other lighting work is reserved; zero retains the fixed-query control.
        // Positive allowance requires GPU timing. It is predictive, not GPU preemption.
        float m_temporal_frame_allowance_ms{};
        bool m_temporal_gpu_queue{true}; // GPU-built mandatory work list and indirect dispatch.
        bool m_temporal_serial_reference{}; // Bounded correctness control; never selected by game routing.
        bool m_temporal_skip_table{}; // R8UI traversal experiment; retained only after matched measurements.
        // Injected logical sampling time; prepare-time age is conservative while batches complete.
        // Appended to retain existing positional settings initialization.
        u64 m_temporal_sample_time_ns{};
        // Native receiver region to certify after updating; all-zero requests the full source.
        WorldRadianceReceiverWindow m_temporal_coverage_window{};
        // Optional native refresh tile sharing this source, setup, and parent work.
        std::optional<WorldRadianceReceiverWindow> m_temporal_refresh_window{};
        // Optional fine samples on the GPU queue. The scheduler always budgets the
        // complete candidate extent; an invalid probe remains mandatory.
        bool m_prioritize_convergence{};
        std::optional<WorldRadianceGameplayDemand> m_gameplay_demand{};
    };

    struct WorldRadianceDemand {
        std::array<u64, 2> m_parity_bytes{};
        u64 m_refined_bytes{}, m_probe_state_bytes{}, m_source_gpu_bytes{}, m_readback_bytes{}, m_root_cpu_bytes{};
        u64 m_candidate_bytes{}, m_peak_bytes{}, m_largest_resource_bytes{};
        u64 m_query_upper_bound{}, m_expected_rows{};
        std::error_code m_admission_error{};
    };

    class WorldRadianceGeneration {
    public:
        [[nodiscard]] const std::shared_ptr<const WorldRadianceSource> &source() const noexcept;
        [[nodiscard]] TerrainReceiverCohort cohort() const noexcept;
        [[nodiscard]] rhi::IBuffer *coefficientsBuffer() const noexcept;
        [[nodiscard]] rhi::IBuffer *probeStateBuffer() const noexcept;
        [[nodiscard]] u64 probeStateBytes() const noexcept;
        [[nodiscard]] u64 transportVersion() const noexcept;
        [[nodiscard]] i32 skyHour() const noexcept;
        [[nodiscard]] const std::shared_ptr<const WorldRadianceGameplayField> &gameplayField() const noexcept;
        [[nodiscard]] const WorldRadianceStatistics &statistics() const noexcept;
        [[nodiscard]] std::array<i32, 2> probeMinimum() const noexcept;
        [[nodiscard]] std::array<u32, 2> probeCounts() const noexcept;
        // Probe-space coverage recomputed from this source. Other rows may retain older history.
        [[nodiscard]] WorldRadianceReceiverWindow updatedProbeWindow() const noexcept;
        [[nodiscard]] bool supplementalRefreshYielded() const noexcept;
        // Native receiver coverage whose complete interpolation support matches this source version.
        // Empty means that the requested coverage is not yet current, even if a field was published.
        [[nodiscard]] WorldRadianceReceiverWindow currentReceiverWindow() const noexcept;
        [[nodiscard]] WorldRadianceRefinedSpacing refinedSpacing() const noexcept;
        [[nodiscard]] WorldRadianceTransport transport() const noexcept;
        [[nodiscard]] float maximumResponse() const noexcept;
        [[nodiscard]] u64 bufferBytes() const noexcept;
        [[nodiscard]] bool matchesDevice(const rhi::IDevice &device) const noexcept;
        [[nodiscard]] u64 bounceEpoch() const noexcept;
        [[nodiscard]] FenceCompletionValue completionFence() const noexcept;
        [[nodiscard]] std::optional<double> maximumGpuMilliseconds() const noexcept;

    private:
        friend class WorldRadiancePass;
        std::shared_ptr<const WorldRadianceSource> m_source;
        std::shared_ptr<const WorldRadianceGameplayField> m_gameplay_field;
        rhi::ComPtr<rhi::IBuffer> m_coefficients, m_probe_states;
        rhi::IDevice *m_device{};
        TerrainReceiverCohort m_cohort{};
        WorldRadianceStatistics m_statistics{};
        u64 m_bytes{}, m_bounce_epoch{}, m_transport_version{};
        FenceCompletionValue m_completion{kFenceValueNone};
        std::optional<double> m_max_gpu_milliseconds{};
        WorldRadianceReceiverWindow m_updated_probe_window{}, m_current_receiver_window{};
        std::array<i32, 2> m_probe_min{};
        std::array<u32, 2> m_probe_counts{};
        float m_maximum_response{};
        WorldRadianceRefinedSpacing m_refined_spacing{WorldRadianceRefinedSpacing::two_cell_centres};
        WorldRadianceTransport m_transport{WorldRadianceTransport::cascade_diagnostic};
        i32 m_sky_hour{-1};
        bool m_refresh_yielded{};
    };

    // ------------------------------------------------------------------
    // opt-in single diffuse reflection, generation-local material/face storage
    // ------------------------------------------------------------------

    struct WorldRadianceBounceSettings {
        u64 m_budget_bytes{}, m_reserve_bytes{}, m_max_resource_bytes{}, m_request_epoch{};
        bool m_measure_gpu{true};
    };

    // Diagnostic support counts only: no bins/queries or scheduling are changed.
    // Work-frame counts are optimistic lower bounds, never measured cadence.
    struct WorldRadianceSupportCounts {
        u64 m_direct_baseline{}, m_direct_included{}, m_moment_baseline{}, m_moment_included{};
        u64 m_direct_quanta{}, m_moment_quanta{}, m_projection_quanta{}, m_discovery_quanta{}, m_other_bounce_quanta{};
        u64 m_packed_direct_quanta{}, m_packed_moment_quanta{}, m_optimistic_work_frames{};
        u64 m_emitter_bounds{}, m_support_tests{}, m_cpu_key_capacity_bytes{}, m_key_readback_bytes{};
        TimeSpan m_cpu_elapsed{};
        bool m_cpu_measured{};
    };

    struct WorldRadianceBounceDemand {
        u64 m_faces{}, m_lookup_capacity{}, m_moment_bytes{}, m_lookup_bytes{};
        u64 m_indirect_bytes{}, m_combined_bytes{}, m_parity_bytes{}, m_trace_upper_bound{}, m_record_count{};
        u64 m_owned_bytes{}, m_peak_bytes{}, m_largest_resource_bytes{};
        std::optional<WorldRadianceSupportCounts> m_support_counts{};
        u32 m_batches{};
        std::error_code m_admission_error{};
    };

    // ------------------------------------------------------------------
    // pass-owned producer on the existing render-owner queue and fence
    // ------------------------------------------------------------------

    enum class WorldRadianceTimingStatus : u8 {
        disabled,
        pending,
        ready,
        unsupported,
        failed,
        retired_unresolved,
        not_found
    };

    // CPU observation only. Published generation objects remain immutable.
    // Scope: encode's clear/transitions/compute/statistics copy, plus explicitly
    // enabled coefficient copy; excludes prepare/allocation/source uploads.
    struct WorldRadianceTiming {
        TerrainReceiverCohort m_cohort{};
        std::error_code m_error{};
        std::optional<double> m_milliseconds{};
        FenceCompletionValue m_submission{kFenceValueNone};
        WorldRadianceTimingStatus m_status{WorldRadianceTimingStatus::not_found};
    };

    // ------------------------------------------------------------------
    // complete generation-scoped reflected-contributor discovery, not lighting
    // ------------------------------------------------------------------

    struct WorldRadianceDiscoverySettings {
        u64 m_budget_bytes{}, m_reserve_bytes{}, m_max_resource_bytes{};
        u64 m_request_epoch{};
        WorldRadianceReceiverWindow m_window{};
        // Zero selects the checked terminal-plus-partial-face bound. A smaller limit is
        // useful for bounded admission/overflow controls; overflow always fails.
        u32 m_key_capacity_limit{};
        bool m_measure_gpu{true};
        bool m_readback_keys{}; // Bounded fixture/support evidence; all CPU/readback capacities charged first.
    };

    struct WorldRadianceDiscoveryDemand {
        u64 m_trace_upper_bound{}, m_record_count{}, m_key_capacity{};
        u64 m_bitset_bytes{}, m_key_bytes{}, m_owned_bytes{}, m_peak_bytes{}, m_largest_resource_bytes{};
        u32 m_batches{};
        std::error_code m_admission_error{};
    };

    class WorldRadianceFaceDiscovery {
    public:
        [[nodiscard]] const std::shared_ptr<const WorldRadianceSource> &source() const noexcept;
        [[nodiscard]] TerrainReceiverCohort cohort() const noexcept;
        [[nodiscard]] WorldRadianceReceiverWindow window() const noexcept;
        [[nodiscard]] u64 requestEpoch() const noexcept;
        [[nodiscard]] u32 faceCount() const noexcept;
        [[nodiscard]] u64 faceHitOccurrences() const noexcept;
        [[nodiscard]] u32 conservativeOffscreenFaceCount() const noexcept;
        [[nodiscard]] rhi::IBuffer *keysBuffer() const noexcept;
        [[nodiscard]] u64 bufferBytes() const noexcept;
        [[nodiscard]] FenceCompletionValue completionFence() const noexcept;
        [[nodiscard]] const WorldRadianceStatistics &statistics() const noexcept;
        [[nodiscard]] std::optional<double> maximumGpuMilliseconds() const noexcept;

    private:
        friend class WorldRadiancePass;
        std::shared_ptr<const WorldRadianceSource> m_source;
        std::vector<u32> m_support_keys; // Owned immutable diagnostic snapshot after the exact final fence.
        rhi::ComPtr<rhi::IBuffer> m_bits, m_keys;
        std::optional<double> m_max_gpu_milliseconds{};
        TerrainReceiverCohort m_cohort{};
        WorldRadianceStatistics m_statistics{};
        WorldRadianceReceiverWindow m_window{};
        u64 m_request_epoch{}, m_bytes{}, m_face_hit_occurrences{}, m_work_quanta{};
        FenceCompletionValue m_completion{kFenceValueNone};
        u32 m_count{}, m_offscreen_count{};
        bool m_support_requested{}, m_support_keys_ready{}, m_support_explicit{};
    };

    // ------------------------------------------------------------------
    // pass-owned producer on the existing render-owner queue and fence
    // ------------------------------------------------------------------

    struct WorldRadianceMatterSettings {
        u64 m_budget_bytes{}, m_reserve_bytes{}, m_max_resource_bytes{}, m_request_epoch{};
        bool m_measure_gpu{true};
    };

    struct WorldRadianceMatterDemand {
        u64 m_input_rows{}, m_overlay_capacity{}, m_lookup_capacity{};
        u64 m_snapshot_bytes{}, m_sort_bytes{}, m_scan_bytes{}, m_overlay_bytes{};
        u64 m_lookup_bytes{}, m_occupancy_bytes{}, m_palette_bytes{}, m_readback_bytes{}, m_input_lease_bytes{};
        u64 m_owned_bytes{}, m_peak_bytes{}, m_largest_resource_bytes{};
        u32 m_batches{};
        std::error_code m_admission_error{};
    };

    class WorldRadiancePass {
    public:
        WorldRadiancePass();
        ~WorldRadiancePass();

        WorldRadiancePass(const WorldRadiancePass &) = delete;
        WorldRadiancePass &operator=(const WorldRadiancePass &) = delete;

        [[nodiscard]] std::error_code initialize(
            rhi::IDevice &device, IShaderService &shaders, const std::filesystem::path &content_root);
        [[nodiscard]] Expected<WorldRadianceDemand> prepare(
            std::shared_ptr<const WorldRadianceSource> source, TerrainReceiverCohort cohort,
            WorldRadianceSettings settings, FenceCompletionValue completed);
        [[nodiscard]] std::error_code encode(const PreRenderContext &context, u64 actual_frame = 0u);
        [[nodiscard]] std::error_code notifySubmitted(FenceCompletionValue submitted);
        [[nodiscard]] std::error_code discardUnsubmitted();
        [[nodiscard]] std::error_code notifySubmitFailed();
        // Collect one accepted strip/copy quantum's fence/status. Fresh strips
        // use the admitted receiver quota (up to1024); copies can cover more rows.
        // Publication pages remain 32x32. Never submits the next quantum itself.
        [[nodiscard]] Expected<bool> advance(FenceCompletionValue completed);
        // Rebudget unsubmitted adaptive work. False means defer this frame without consuming work.
        [[nodiscard]] Expected<bool> budgetTemporalFrame(float available_milliseconds, bool allow_refresh = true);
        [[nodiscard]] bool canEncodeNextBatch() const noexcept;
        // Adaptive temporal mode returns a remaining-work upper bound; grouping changes with completed timings.
        [[nodiscard]] u32 pendingBatches() const noexcept;
        [[nodiscard]] Expected<std::shared_ptr<const WorldRadianceGeneration>> acquire(FenceCompletionValue completed);

        // Never waits or submits. A completed batch may still have pending
        // backend host readiness; replacement retires that observation explicitly.
        [[nodiscard]] WorldRadianceTiming pollTiming(TerrainReceiverCohort cohort, FenceCompletionValue completed);

        [[nodiscard]] Expected<std::vector<WorldRadianceCoefficients>> readDiagnosticCoefficients(
            FenceCompletionValue completed);
        [[nodiscard]] Expected<std::vector<WorldRadianceProbeState>> readDiagnosticProbeStates(
            FenceCompletionValue completed);

        // Prepares only the immutable discovery program/pipeline pair on its
        // initialized owner thread. Repeated calls do no GPU work or submission.
        [[nodiscard]] std::error_code prepareDiscoveryPipeline();
        [[nodiscard]] std::error_code prepareMatterPipeline();

        // Discovery borrows the completed same-source GPU input by owning
        // ComPtr copies. It never replaces or mutates the lighting generation.
        [[nodiscard]] Expected<WorldRadianceDiscoveryDemand> prepareDiscovery(
            std::shared_ptr<const WorldRadianceSource> source, TerrainReceiverCohort cohort,
            WorldRadianceDiscoverySettings settings, FenceCompletionValue completed);
        [[nodiscard]] std::error_code encodeDiscovery(const PreRenderContext &context, u64 actual_frame);
        [[nodiscard]] bool canEncodeDiscovery() const noexcept;
        [[nodiscard]] Expected<bool> advanceDiscovery(FenceCompletionValue completed);
        [[nodiscard]] Expected<std::shared_ptr<const WorldRadianceFaceDiscovery>> acquireDiscovery(
            FenceCompletionValue completed);
        [[nodiscard]] Expected<bool> cancelDiscovery(FenceCompletionValue completed);
        [[nodiscard]] Expected<std::vector<u32>> readDiscoveryKeys(FenceCompletionValue completed);

        // Requires a completed exact discovery and its pristine direct donor.
        // Does not mutate/publish through acquire(); only acquireBounce() exposes
        // a complete combined result with the exact source/cohort/window/epoch.
        [[nodiscard]] Expected<WorldRadianceBounceDemand> prepareBounce(
            std::shared_ptr<const WorldRadianceFaceDiscovery> discovery, WorldRadianceBounceSettings settings,
            FenceCompletionValue completed, const ITimerClock *diagnostic_clock = nullptr);
        [[nodiscard]] std::error_code encodeBounce(const PreRenderContext &context, u64 actual_frame);
        [[nodiscard]] bool canEncodeBounce() const noexcept;
        [[nodiscard]] Expected<bool> advanceBounce(FenceCompletionValue completed);
        [[nodiscard]] Expected<std::shared_ptr<const WorldRadianceGeneration>> acquireBounce(
            FenceCompletionValue completed);
        [[nodiscard]] Expected<bool> cancelBounce(FenceCompletionValue completed);

        // The first accepted quantum freezes ALL selected original rows. Later
        // grouping never rereads mutable Hot; stale pre-encode views are superseded.
        [[nodiscard]] Expected<WorldRadianceMatterDemand> prepareMatter(
            std::shared_ptr<const WorldRadianceSource> terrain, GridParticleGpuView particles,
            WorldRadianceMatterSettings settings, FenceCompletionValue completed);
        [[nodiscard]] std::error_code encodeMatter(
            const PreRenderContext &context, u64 actual_frame, float grouping_allowance_ms = 0.0f);
        [[nodiscard]] bool canEncodeMatter() const noexcept;
        [[nodiscard]] Expected<bool> advanceMatter(FenceCompletionValue completed);
        [[nodiscard]] Expected<std::shared_ptr<const WorldRadianceSource>> acquireMatter(
            FenceCompletionValue completed);
        [[nodiscard]] Expected<bool> cancelMatter(FenceCompletionValue completed);

        [[nodiscard]] std::error_code shutdown(const GpuQuiescenceResult &proof);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
}
