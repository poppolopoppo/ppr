export module engine.app:renderer.post_process_pass;

import :renderer.frame_fence;
import :renderer.presentation;
import :renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // independent post-processing owner
    // ------------------------------------------------------------------
    // Consumes an immutable resolved scene-linear HDR view and owns exposure
    // adaptation, bloom and the single AgX display treatment. It has no scene
    // callback, AA mode or reconstruction-history policy: reconstruction owns
    // its samples/history and supplies the resolved image. The caller renders
    // content through its own stage A, evaluates outputTexture() for stage B's
    // own R-sized attachment, then presents the display-linear result at fixed
    // pixelization through stage C. Native UI follows presentation.

    class PostProcessPass final {
    public:
        // Mirrored field-for-field by luminance_histogram.slang / auto_exposure.slang;
        // the CPU functions below are the exact oracle for the GPU reduction.
        static constexpr u32 kHistogramBins{64u};
        static constexpr u32 kMaxLuminanceExtent{256u};
        static constexpr float kLog2LuminanceMin{-8.0f};
        static constexpr float kLog2LuminanceMax{8.0f};
        static constexpr float kLowPercentile{0.10f};
        static constexpr float kHighPercentile{0.90f};
        static constexpr float kMiddleGrayLuminance{0.18f};

        // Bloom level geometry and level weights. Bloom level 0 is ceil(R/2);
        // each later level is ceil(previous/2), stopping at 1x1 or six levels.
        static constexpr u32 kMaxBloomLevels{6u};
        static constexpr float kBloomSoftKnee{0.5f};
        // Mirrored by spd_bloom.slang. The descent multiplies each unweighted
        // level by its weight exactly once.
        static constexpr std::array<float, kMaxBloomLevels> kBloomLevelWeights{1.0f, 0.5f, 0.25f, 0.15f, 0.1f, 0.05f};

        // Mirrored by present_scene.slang: the cross-tap coefficient magnitude at
        // the low and high ends of the strength ramp, the local-contrast
        // reciprocal's floor, and the bound that keeps the 5-tap kernel well
        // conditioned on black input.
        static constexpr float kSharpenPeakMin{0.125f};
        static constexpr float kSharpenPeakMax{0.25f};
        static constexpr float kSharpenContrastEps{1.0e-3f};
        static constexpr float kSharpenMaxWeight{2.0f};

        [[nodiscard]] static float luminance(float r, float g, float b) noexcept;
        [[nodiscard]] static float log2Luminance(float lum) noexcept;
        [[nodiscard]] static u32 luminanceBin(float log2_lum) noexcept;
        [[nodiscard]] static float histogramBinCenterLog2(u32 bin) noexcept;
        // Fraction of the total texel count retained by the 10-90% trim. Counts
        // stay fractional; a one-texel histogram retains exactly 0.8.
        [[nodiscard]] static float retainedMass(std::span<const u32> histogram, u32 total_texels) noexcept;
        [[nodiscard]] static float averageLog2Luminance(std::span<const u32> histogram, u32 total_texels) noexcept;
        [[nodiscard]] static float targetEv(float average_log2_lum, float ev_min, float ev_max) noexcept;
        [[nodiscard]] static float adaptationAlpha(TimeSpan dt, float tau) noexcept;

        [[nodiscard]] static int2 bloomLevelExtent(int2 resolved_extent, u32 level) noexcept;
        [[nodiscard]] static u32 bloomLevelCount(int2 resolved_extent) noexcept;
        // Full six-entry chain; entries after bloomLevelCount are the final
        // clamped 1x1 level so callers can size a fixed cohort.
        [[nodiscard]] static std::array<int2, kMaxBloomLevels> bloomLevelExtents(int2 resolved_extent) noexcept;

        // Per-channel oracle for the presentation-resolution CAS in
        // present_scene.slang. The shader applies exactly these expressions to
        // R, G and B independently on five resolved taps: `center` plus the four
        // resolved neighbours one texel apart (extent-clamped). strength <= 0 is
        // the identity.
        [[nodiscard]] static int2 resolvedPixelForWindowPixel(
            int2 window_pixel, int2 resolved_extent, u32 pixel_scale) noexcept;
        [[nodiscard]] static float sharpenWeight(float cross_min, float cross_max) noexcept;
        [[nodiscard]] static float sharpenChannel(
            float center, float left, float right, float up, float down, float strength) noexcept;

        [[nodiscard]] std::error_code initialize(
            IShaderService &shader_service, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code shutdown();
        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        // Owner-lane frame entry. Selects and allocates the output cohort before
        // the caller evaluates outputTexture(); commits no exposure state.
        // Pending output-slot exhaustion reports transient capacity without a
        // steady-state CPU wait.
        [[nodiscard]] std::error_code beginFrame(
            rhi::IDevice &device, int2 resolved_extent, GridWorldIdentity world, FenceCompletionValue completed);

        // Validates the resolved binding against the selected output, retains it
        // through B completion, and encodes metering/adaptation/bloom. The view
        // may carry backing larger than its valid content rectangle.
        [[nodiscard]] std::error_code prepare(
            const PreRenderContext &context, const ResolvedHdrView &view, const PostProcessSettings &settings,
            TimeSpan active_elapsed);

        [[nodiscard]] rhi::ITexture &outputTexture() const;

        // Encodes the single AgX treatment into this pass's own R-sized output
        // attachment (one RGBA16F color attachment, no depth).
        [[nodiscard]] std::error_code render(const DrawContext &context);
        // Fixed-P nearest enlargement/crop from the display-linear output to the
        // acquired surface. Samples exact integer resolved pixels; never
        // UV-stretches. The same pass sharpens the display-linear content on
        // resolved texels (cross taps one texel apart) so the nearest enlargement
        // replicates each sharpened texel to its whole pixel-scale block, then
        // applies the single sRGB transfer.
        [[nodiscard]] std::error_code present(const DrawContext &context, const PresentationGrid &grid);
        // A previously accepted stage-B output that a retained-picture stage C
        // may present without a new stage B: same resolved extent and world.
        [[nodiscard]] bool retainedPictureAvailable(int2 resolved_extent, GridWorldIdentity world) const noexcept;
        // Selects that accepted output as stage C's source. Commits no lighting,
        // history or exposure state; acceptance of a later B replaces it.
        void selectRetainedPicture(int2 resolved_extent) noexcept;
        // Status-to-readback seam; the cleanup path exposes no numeric status.
        [[nodiscard]] std::error_code finalize(const PreRenderContext &context);

        // Accepted stage-B settlement: pins the output writer through the value
        // and commits the exposure role/active-time baseline.
        void notifySubmitted(FenceCompletionValue submit_value) noexcept;
        // Extends the selected output's reader fence through stage C and any
        // retained-picture draw. CPU acknowledgment alone never releases it.
        void notifyPresented(FenceCompletionValue present_value) noexcept;
        // Definitely unsubmitted work commits nothing; the retained view and
        // pending exposure are dropped.
        void discardUnsubmitted() noexcept;
        [[nodiscard]] std::error_code collectCompleted(FenceCompletionValue completed);

        // Tests-only observation of the committed exposure role. It moves only
        // on an accepted stage B, so an accepted resolve followed by a rejected
        // one must leave it unchanged.
        struct ExposureRoleForTest {
            GridWorldIdentity m_world{};
            TimeSpan m_active_baseline{};
            u32 m_slot{};
            bool m_valid{};
        };
        [[nodiscard]] ExposureRoleForTest exposureRoleForTest() const noexcept {
            return ExposureRoleForTest{
                .m_world = m_exposure_world,
                .m_active_baseline = m_active_baseline,
                .m_slot = m_exposure_slot,
                .m_valid = m_exposure_valid,
            };
        }

        // Read-only committed 1x1 adapted-exposure offset slot. The caller must
        // prove GPU completion before reading.
        [[nodiscard]] rhi::ITexture *exposureTextureForTest() const noexcept {
            return m_exposure_valid and m_exposure_slot < kFrameRingSlots ? m_slots[m_exposure_slot].m_exposure.get()
                                                                          : nullptr;
        }

        // Settings retained by the most recent successful prepare. A value-only
        // positive control that an edit actually reached this pass, independent
        // of auto-exposure adaptation noise.
        [[nodiscard]] PostProcessSettings lastPreparedSettingsForTest() const noexcept {
            return m_pending_settings;
        }

    private:
        // ------------------------------------------------------------------
        // per-output-slot cohort
        // ------------------------------------------------------------------
        // One completion-governed output cohort. The output, bloom chain,
        // metering storage and root objects are replaced together on extent
        // change; the 1x1 exposure state survives resize so adaptation is
        // independent of sample/history grid changes.
        struct OutputSlot {
            rhi::ComPtr<rhi::ITexture> m_output{};
            rhi::ComPtr<rhi::ITextureView> m_output_view{};
            rhi::ComPtr<rhi::ITexture> m_luminance{};
            rhi::ComPtr<rhi::ITextureView> m_luminance_view{};
            rhi::ComPtr<rhi::IBuffer> m_histogram{};
            rhi::ComPtr<rhi::IBuffer> m_target_ev{};
            std::array<rhi::ComPtr<rhi::ITexture>, kMaxBloomLevels> m_bloom{};
            std::array<rhi::ComPtr<rhi::ITextureView>, kMaxBloomLevels> m_bloom_views{};
            rhi::ComPtr<rhi::ITexture> m_exposure{};
            rhi::ComPtr<rhi::ITextureView> m_exposure_view{};
            rhi::ComPtr<rhi::IShaderObject> m_post_root{};
            rhi::ComPtr<rhi::IShaderObject> m_present_root{};
            std::array<rhi::ComPtr<rhi::IShaderObject>, 4> m_histogram_roots{};
            std::array<rhi::ComPtr<rhi::IShaderObject>, 3> m_bloom_roots{};
            rhi::ComPtr<rhi::IShaderObject> m_auto_exposure_root{};
            ResolvedHdrView m_retained_view{};
            FenceCompletionValue m_retained_submission{kFenceValueNone};
            FenceCompletionValue m_last_use{kFenceValueNone};
            int2 m_extent{zero_v};
            u32 m_bloom_levels{};
        };

        // ------------------------------------------------------------------
        // program and cohort creation
        // ------------------------------------------------------------------
        [[nodiscard]] std::error_code createPrograms_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code createSlot_(
            rhi::IDevice &device, OutputSlot &slot, int2 resolved_extent, bool create_cohort);
        [[nodiscard]] std::error_code createExposure_(rhi::IDevice &device, OutputSlot &slot);
        [[nodiscard]] std::error_code ensurePresentPipeline_(
            rhi::IDevice &device, const RenderPipelineKey &surface_key);

        [[nodiscard]] std::error_code encodeMetering_(const PreRenderContext &context, OutputSlot &slot);
        [[nodiscard]] std::error_code encodeBloom_(const PreRenderContext &context, OutputSlot &slot);

        void releaseGpuResources_() noexcept;

        // ------------------------------------------------------------------
        // program, sampler and pipeline state
        // ------------------------------------------------------------------

        rhi::ComPtr<rhi::IShaderProgram> m_post_program{};
        rhi::ComPtr<rhi::IRenderPipeline> m_post_pipeline{};
        rhi::ComPtr<rhi::IShaderProgram> m_present_program{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 4> m_histogram_programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 4> m_histogram_pipelines{};
        rhi::ComPtr<rhi::IShaderProgram> m_auto_exposure_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_auto_exposure_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 3> m_bloom_programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 3> m_bloom_pipelines{};
        rhi::ComPtr<rhi::ISampler> m_linear_sampler{};
        rhi::DescriptorHandle m_linear_sampler_handle{};
        rhi::ComPtr<rhi::ITexture> m_empty_exposure{};
        rhi::ComPtr<rhi::ITextureView> m_empty_exposure_view{};

        // Surface presentation pipeline keyed by the acquired target signature.
        // A signature change defers the old pipeline instead of dropping it
        // mid-flight.
        std::optional<RenderPipelineKey> m_present_key{};
        rhi::ComPtr<rhi::IRenderPipeline> m_present_pipeline{};

        // ------------------------------------------------------------------
        // pass state
        // ------------------------------------------------------------------

        // Injected service outlives this pass, including lazy first-use work.
        safe_ptr<IShaderService> m_shader_service{};
        std::filesystem::path m_content_dir{};
        DeferredReleaseQueue m_deferred_releases{};
        FenceCompletionValue m_last_submitted_value{kFenceValueNone};

        std::array<OutputSlot, kFrameRingSlots> m_slots{};
        u32 m_cursor{0u};
        u32 m_selected{0u};
        u32 m_presented_slot{0u};
        int2 m_resolved_extent{zero_v};
        GridWorldIdentity m_frame_world{};

        // Committed exposure role: the last accepted slot's 1x1 offset and the
        // world/active-time baseline it belongs to. Rejected attempts never
        // replace these.
        u32 m_exposure_slot{0u};
        bool m_exposure_valid{};
        GridWorldIdentity m_exposure_world{};
        TimeSpan m_active_baseline{};
        bool m_active_baseline_valid{};

        // Pending settlement for the current prepared frame.
        PostProcessSettings m_pending_settings{};
        TimeSpan m_pending_active_elapsed{};
        bool m_pending_exposure_read{};
        u32 m_pending_exposure_read_slot{0u};
        bool m_prepared{};
        bool m_rendered{};
        bool m_ready{};
    };
}
