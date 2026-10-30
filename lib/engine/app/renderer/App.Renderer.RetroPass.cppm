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
        u32 m_pixel_scale{3u};
    };

    class RetroPass final {
    public:
        static constexpr u32 kMinPixelScale{2u};
        static constexpr u32 kMaxPixelScale{8u};
        static constexpr u32 kPaletteSize{32u};

        [[nodiscard]] static u32 clampPixelScale(u32 scale) noexcept;
        [[nodiscard]] static int2 sceneExtent(int2 native_extent, u32 pixel_scale) noexcept;
        [[nodiscard]] static RenderPipelineSignature scenePipelineSignature(bool temporal = false) noexcept;
        [[nodiscard]] static float2 temporalJitter(u32 phase) noexcept;

        // Called once per admitted frame on the render owner. Only accepted
        // submissions advance history and the four-phase sample pattern.
        [[nodiscard]] CameraSnapshot beginFrame(
            const RetroSettings &settings, const CameraSnapshot &camera, int2 extent, u64 world_generation,
            u64 world_job = 0u, u64 world_content = 0u);
        [[nodiscard]] bool temporalActive() const noexcept {
            return m_settings.m_temporal_enabled;
        }

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code shutdown();
        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        [[nodiscard]] std::error_code prepareScene(
            const PreRenderContext &context, const RenderPipelineSignature &scene_signature, int2 extent,
            DrawCallback encode_scene);

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
            rhi::ComPtr<rhi::ITexture> m_history{};
            rhi::ComPtr<rhi::ITextureView> m_history_view{};
            FenceCompletionValue m_last_use{kFenceValueNone};
            int2 m_extent{zero_v};
            bool m_temporal{};
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


        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        CameraSnapshot m_camera{};
        float4x4 m_previous_view_projection{identity_v};

        // CPU-side DawnBringer-32 palette (normalized sRGB bytes), copied into
        // the per-draw RetroParameters constant for the selected scene slot.
        std::array<float4, kPaletteSize> m_palette;

        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_temporal_program{};
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
        int2 m_frame_extent{zero_v};

        RetroSettings m_settings{.m_palette_enabled = true};
        u32 m_scene_cursor{0u};
        u32 m_scene_slot{0u};
        u32 m_history_slot{};
        u32 m_phase{};
        bool m_history_valid{};
        bool m_temporal_supported{};
        bool m_warned_unsupported{};
        bool m_history_used{};

        bool m_scene_ready{false};
        bool m_scene_recorded{false};
        bool m_ready{false};
    };
}
