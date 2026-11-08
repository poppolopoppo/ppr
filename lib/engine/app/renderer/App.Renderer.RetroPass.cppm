export module engine.app:renderer.retro_pass;

import :renderer.frame_fence;
import :renderer.types;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // scene sampling and independent picture treatments
    // ------------------------------------------------------------------
    // The scene renders at native or pixelized resolution through a fenced
    // three-slot ring. Optional temporal reconstruction consumes raw 4x MSAA
    // samples before the independent palette treatment and nearest upscale.
    // The caller supplies content through a synchronous draw callback; this
    // pass owns only sampling, history and presentation resources. UI stays native.

    struct RetroSettings {
        bool m_enabled{false};
        bool m_palette_enabled{false};
        bool m_temporal_enabled{false};
        bool m_hdr_enabled{false};
        bool m_spatial_enabled{false};
        u32 m_pixel_scale{3u};
        float m_exposure_ev{0.0f};
        // Requires scene-linear HDR with spatial or temporal2x reconstruction.
        bool m_bloom_enabled{false};
        bool m_temporal_2x_enabled{false};
        // Histogram eye adaptation: temporal2x-resolved HDR only. The adapted
        // EV is an offset around m_exposure_ev, clamped to [m_ev_min, m_ev_max].
        bool m_auto_exposure{false};
        float m_adaptation_tau{0.7f};
        float m_ev_min{-4.0f};
        float m_ev_max{4.0f};
    };

    // Actual immutable lighting publication used by this scene. The existing
    // world/job/content/revision arguments identify its matching cohort.
    struct RetroLightingPublication {
        FenceCompletionValue m_completion{kFenceValueNone};
        u64 m_bounce_epoch{};
        std::array<i32, 2> m_probe_minimum{};
        std::array<u32, 2> m_probe_counts{};

        [[nodiscard]] bool operator==(const RetroLightingPublication &) const noexcept = default;
    };

    // Accepted fresh temporal2x encoding, without resource ownership. A bound
    // history was CPU-eligible and its real SRV was bound; this does not measure
    // per-pixel history influence. No history fence means the read slot is absent.
    struct RetroTemporal2xSubmission {
        RetroLightingPublication m_lighting_publication{};
        FenceCompletionValue m_submission{kFenceValueNone};
        FenceCompletionValue m_history_submission{kFenceValueNone};
        int2 m_raster_extent{zero_v};
        int2 m_output_extent{zero_v};
        u32 m_phase{};
        u32 m_history_write_slot{};
        u32 m_history_read_slot{};
        bool m_history_bound{};
    };

    /// Identity of the pass-owned completed picture. No GPU views escape the
    /// owner; retention protects its slot and every repeated draw adds a reader fence.
    struct RetroCompletedPicture {
        CameraSnapshot m_camera{};
        std::array<u64, 3> m_world_identity{}; // generation, job, content
        TimeSpan m_request_elapsed{};
        int2 m_output_extent{zero_v};
        u64 m_lighting_revision{};
        RetroLightingPublication m_lighting_publication{};
    };

    class RetroPass final {
    public:
        static constexpr u32 kMinPixelScale{2u};
        static constexpr u32 kMaxPixelScale{8u};
        static constexpr u32 kPaletteSize{32u};

        // Histogram eye-adaptation reduction constants, mirrored field-for-field
        // by luminance_histogram.slang. The CPU functions below are the exact
        // oracle the shader's log-bin/percentile reduction must match.
        static constexpr u32 kHistogramBins{64u};
        static constexpr u32 kLuminanceExtent{64u};
        static constexpr float kLog2LuminanceMin{-8.0f};
        static constexpr float kLog2LuminanceMax{8.0f};
        static constexpr float kHistogramLowPercentile{0.10f};
        static constexpr float kHistogramHighPercentile{0.90f};
        static constexpr float kMiddleGrayLuminance{0.18f};

        [[nodiscard]] static u32 clampPixelScale(u32 scale) noexcept;

        // CPU mirror of the histogram reduction (tested directly, and the GPU
        // smoke oracle). log2Luminance clamps zero to the representable floor so
        // a black frame stays finite; targetEv clamps to [ev_min, ev_max].
        [[nodiscard]] static float luminance(float r, float g, float b) noexcept;
        [[nodiscard]] static float log2Luminance(float lum) noexcept;
        [[nodiscard]] static u32 luminanceBin(float log2_lum) noexcept;
        [[nodiscard]] static float histogramBinCenterLog2(u32 bin) noexcept;
        [[nodiscard]] static float averageLog2Luminance(std::span<const u32> histogram, u32 total_pixels) noexcept;
        [[nodiscard]] static float targetEv(float average_log2_lum, float ev_min, float ev_max) noexcept;
        // Exponential-approach blend for a frame delta and time constant. A
        // non-positive delta or tau yields zero (no adaptation), never a snap.
        [[nodiscard]] static float adaptationAlpha(TimeSpan dt, float tau) noexcept;
        [[nodiscard]] static int2 sceneExtent(int2 native_extent, u32 pixel_scale) noexcept;
        [[nodiscard]] static RenderPipelineSignature scenePipelineSignature(bool temporal = false) noexcept;
        [[nodiscard]] static RenderPipelineSignature temporal2xScenePipelineSignature() noexcept;
        [[nodiscard]] static float2 temporalJitter(u32 phase) noexcept;

        // Pure preflight; spatial requires positive even output and an exact 2x
        // raster mapping. Bloom requires HDR plus spatial or temporal2x. Other
        // palette/HDR policies remain independent of sample reconstruction.
        [[nodiscard]] static std::error_code validateFrameSettings(
            const RetroSettings &settings, int2 raster_extent, int2 output_extent) noexcept;

        // Called once per admitted frame on the render owner. Only accepted
        // submissions advance history and the four-phase sample pattern.
        // Invalid exposure rejects before changing policy, history or resources.
        // HDR producers supply finite RGBA16F values; negative intermediates
        // remain linear until the display boundary. No texture scan is implied.
        // frame_delta is the per-frame elapsed time driving eye adaptation; it is
        // distinct from request_elapsed (a cumulative request timestamp).
        [[nodiscard]] Expected<CameraSnapshot> beginFrame(
            const RetroSettings &settings, const CameraSnapshot &camera, int2 extent, u64 world_generation,
            u64 world_job = 0u, u64 world_content = 0u, TimeSpan request_elapsed = {}, TimeSpan frame_delta = {},
            u64 lighting_revision = 0u, RetroLightingPublication lighting_publication = {});
        [[nodiscard]] Expected<CameraSnapshot> beginFrame(
            const RetroSettings &settings, const CameraSnapshot &camera, int2 raster_extent, int2 output_extent,
            u64 world_generation, u64 world_job = 0u, u64 world_content = 0u, TimeSpan request_elapsed = {},
            TimeSpan frame_delta = {}, u64 lighting_revision = 0u, RetroLightingPublication lighting_publication = {});
        [[nodiscard]] bool temporalActive() const noexcept {
            return m_settings.m_temporal_enabled;
        }

        [[nodiscard]] bool spatialActive() const noexcept {
            return m_settings.m_spatial_enabled;
        }

        [[nodiscard]] bool temporal2xActive() const noexcept {
            return m_settings.m_temporal_2x_enabled;
        }

        // Historical receipt: consumers join its fence to their accepted fresh
        // submission. Retained draws and rejected attempts leave it unchanged.
        [[nodiscard]] std::optional<RetroTemporal2xSubmission> lastTemporal2xSubmission() const noexcept;

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code shutdown();
        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        [[nodiscard]] std::error_code prepareScene(
            const PreRenderContext &context, const RenderPipelineSignature &scene_signature, int2 extent,
            DrawCallback encode_scene);

        /// Opt-in current-frame (non-temporal) picture retention for asynchronous
        /// scene preparation. Legacy callers keep the original three-slot policy.
        [[nodiscard]] std::error_code enableCompletedPictureRetention();
        [[nodiscard]] std::optional<RetroCompletedPicture> restoreCompletedPicture(FenceCompletionValue completed);

        [[nodiscard]] std::error_code render(const DrawContext &context);
        void notifyFrameSubmitted(FenceCompletionValue submit_value) noexcept;

    private:
        // ------------------------------------------------------------------
        // scene target ring
        // ------------------------------------------------------------------

        // One offscreen scene target: linear RGBA16Float color with matching
        // D32Float depth, optionally multisampled plus single-sample history. m_last_use is the
        // fence value of the last submitted frame that rendered into the slot;
        // the slot is reusable only once completion passes it.
        struct SceneTarget {
            rhi::ComPtr<rhi::ITexture> m_color{};
            rhi::ComPtr<rhi::ITextureView> m_color_view{};
            rhi::ComPtr<rhi::ITexture> m_depth{};
            rhi::ComPtr<rhi::ITextureView> m_depth_view{};
            rhi::ComPtr<rhi::ITexture> m_reactive{};
            rhi::ComPtr<rhi::ITextureView> m_reactive_view{};
            rhi::ComPtr<rhi::ITexture> m_history{};
            rhi::ComPtr<rhi::ITextureView> m_history_view{};
            rhi::ComPtr<rhi::ITexture> m_spatial{};
            rhi::ComPtr<rhi::ITextureView> m_spatial_view{};
            std::array<rhi::ComPtr<rhi::ITexture>, 2> m_bloom{};
            std::array<rhi::ComPtr<rhi::ITextureView>, 2> m_bloom_views{};
            rhi::ComPtr<rhi::ITexture> m_luminance{};
            rhi::ComPtr<rhi::ITextureView> m_luminance_view{};
            rhi::ComPtr<rhi::ITexture> m_exposure{};
            rhi::ComPtr<rhi::ITextureView> m_exposure_view{};
            rhi::ComPtr<rhi::IBuffer> m_histogram{};
            rhi::ComPtr<rhi::IBuffer> m_target_ev{};
            CameraSnapshot m_picture_camera{};
            std::array<u64, 3> m_picture_identity{};
            u64 m_picture_lighting_revision{};
            RetroLightingPublication m_picture_lighting_publication{};
            FenceCompletionValue m_content_submit{kFenceValueNone};
            FenceCompletionValue m_last_use{kFenceValueNone};
            TimeSpan m_picture_request_elapsed{};
            int2 m_extent{zero_v};
            int2 m_output_extent{zero_v};
            RetroSettings m_picture_settings{};
            bool m_temporal{};
            bool m_temporal_2x{};
            bool m_spatial_enabled{};
            bool m_bloom_enabled{};
            bool m_auto_exposure_enabled{};
        };

        // ------------------------------------------------------------------
        // program, pipeline, and target creation
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code createShaderProgram_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code createSceneTarget_(rhi::IDevice &device, SceneTarget &slot, int2 extent);
        [[nodiscard]] std::error_code ensurePipeline_(rhi::IDevice &device, const RenderPipelineSignature &signature);

        // Idempotent, wait-free release of every GPU object this pass owns.
        // Shared by shutdown, notifyDeviceLost, and the initialize rollback.
        void releaseGpuResources_() noexcept;
        [[nodiscard]] std::error_code createTemporalProgram_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code resolveTemporal_(const PreRenderContext &context, u32 selected);
        [[nodiscard]] std::error_code ensureSpatialProgram_(rhi::IDevice &device);
        [[nodiscard]] std::error_code resolveSpatial_(const PreRenderContext &context, u32 selected);
        [[nodiscard]] std::error_code ensureTemporal2xProgram_(rhi::IDevice &device);
        [[nodiscard]] std::error_code resolveTemporal2x_(const PreRenderContext &context, u32 selected);
        [[nodiscard]] std::error_code ensureBloomPrograms_(rhi::IDevice &device);
        [[nodiscard]] std::error_code resolveBloom_(const PreRenderContext &context, u32 selected);
        [[nodiscard]] std::error_code ensureHistogramPrograms_(rhi::IDevice &device);
        [[nodiscard]] std::error_code ensureAutoExposureProgram_(rhi::IDevice &device);
        [[nodiscard]] std::error_code resolveAdaptation_(const PreRenderContext &context, u32 selected);


        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        CameraSnapshot m_camera{};
        float4x4 m_previous_view_projection{identity_v};
        TimeSpan m_request_elapsed{};
        TimeSpan m_frame_delta{};

        // CPU-side DawnBringer-32 palette (normalized sRGB bytes), copied into
        // the per-draw RetroParameters constant for the selected scene slot.
        std::array<float4, kPaletteSize> m_palette;

        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_temporal_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_spatial_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_spatial_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderObject>, kFrameRingSlots> m_spatial_roots{};
        rhi::ComPtr<rhi::IShaderProgram> m_temporal_2x_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_temporal_2x_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderObject>, kFrameRingSlots> m_temporal_2x_roots{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 2> m_bloom_programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 2> m_bloom_pipelines{};
        std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, 2>, kFrameRingSlots> m_bloom_roots{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 4> m_histogram_programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 4> m_histogram_pipelines{};
        std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, 4>, kFrameRingSlots> m_histogram_roots{};
        rhi::ComPtr<rhi::IShaderProgram> m_auto_exposure_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_auto_exposure_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderObject>, kFrameRingSlots> m_auto_exposure_roots{};
        rhi::ComPtr<rhi::ITexture> m_empty_exposure{};
        rhi::ComPtr<rhi::ITextureView> m_empty_exposure_view{};
        // Injected service outlives this pass, including lazy first-use compilation.
        safe_ptr<IShaderService> m_shader_service{};
        std::filesystem::path m_content_dir{};
        rhi::ComPtr<rhi::ITexture> m_empty_history{};
        rhi::ComPtr<rhi::ITextureView> m_empty_history_view{};
        rhi::ComPtr<rhi::IRenderPipeline> m_temporal_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderObject>, kFrameRingSlots> m_temporal_roots{};
        // One root per scene ring slot: g_retro/g_scene bindings stay
        // slot-local, so no submission observes another frame's constants.
        std::array<rhi::ComPtr<rhi::IShaderObject>, kFrameRingSlots> m_scene_roots{};
        rhi::ComPtr<rhi::ISampler> m_sampler{};
        rhi::DescriptorHandle m_sampler_handle{};

        // The present pipeline is keyed by the SURFACE signature it targets,
        // never by the scene signature the offscreen targets use. A signature
        // change defers the old pipeline through m_deferred_releases instead of
        // dropping it mid-flight.
        std::optional<RenderPipelineKey> m_pipeline_key{};
        rhi::ComPtr<rhi::IRenderPipeline> m_pipeline{};

        // Retired pipelines keyed by last-use fence value; reclaimed at every
        // render entry and drained unconditionally at shutdown/device-loss.
        DeferredReleaseQueue m_deferred_releases{};
        FenceCompletionValue m_last_submitted_value{kFenceValueNone};

        std::array<SceneTarget, kFrameRingSlots> m_scene_slots{};
        std::array<u64, 3u> m_content_identity{};
        u64 m_content_lighting_revision{};
        RetroLightingPublication m_lighting_publication{};
        std::optional<RetroTemporal2xSubmission> m_last_temporal_2x_submission{};
        int2 m_frame_extent{zero_v};
        int2 m_output_extent{zero_v};

        RetroSettings m_settings{.m_palette_enabled = true};
        u32 m_scene_cursor{0u};
        u32 m_scene_slot{0u};
        u32 m_history_slot{};
        u32 m_phase{};
        u32 m_frame_phase{};
        bool m_history_valid{};
        bool m_temporal_supported{};
        bool m_spatial_supported{};
        bool m_bloom_supported{};
        bool m_temporal_2x_supported{};
        bool m_warned_unsupported{};
        bool m_history_used{};
        bool m_temporal_2x_history_eligible{};

        u32 m_completed_picture_slot{};
        u32 m_sampled_slot{};
        bool m_retain_completed_picture{};
        bool m_completed_picture_valid{};
        bool m_scene_sampled{};

        bool m_scene_ready{false};
        bool m_scene_recorded{false};
        bool m_ready{false};
    };
}
