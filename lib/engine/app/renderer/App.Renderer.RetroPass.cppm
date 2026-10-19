export module engine.app:renderer.retro_pass;

import :renderer.frame_fence;
import :renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // retro pass: low-resolution scene resolve + palette quantization
    // ------------------------------------------------------------------
    // The scene renders offscreen at sceneExtent(native, pixel_scale) through
    // the pass's completion-governed 3-slot target ring; the surface pass then
    // samples that target with a nearest/clamp sampler and quantizes to the
    // 32-entry DawnBringer-32 palette at the surface resolution. The pass owns
    // its offscreen targets, program roots, sampler, and present pipeline; the
    // caller's encode callback draws the content through the DrawContext this
    // pass hands out, so no content pass is named or imported here.

    struct RetroSettings {
        bool m_enabled{false};
        u32 m_pixel_scale{3u};
    };

    class RetroPass final {
    public:
        static constexpr u32 kMinPixelScale{2u};
        static constexpr u32 kMaxPixelScale{8u};
        static constexpr u32 kPaletteSize{32u};

        [[nodiscard]] static u32 clampPixelScale(u32 scale) noexcept;
        [[nodiscard]] static int2 sceneExtent(int2 native_extent, u32 pixel_scale) noexcept;
        [[nodiscard]] static RenderPipelineSignature scenePipelineSignature() noexcept;

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

        // One offscreen scene target: a low-resolution linear RGBA16Float
        // resolve buffer plus its matching D32Float depth. m_last_use is the
        // fence value of the last submitted frame that rendered into the slot;
        // the slot is reusable only once completion passes it.
        struct SceneTarget {
            rhi::ComPtr<rhi::ITexture> m_color{};
            rhi::ComPtr<rhi::ITextureView> m_color_view{};
            rhi::ComPtr<rhi::ITexture> m_depth{};
            rhi::ComPtr<rhi::ITextureView> m_depth_view{};
            int2 m_extent{zero_v};
            FenceCompletionValue m_last_use{kFenceValueNone};
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

        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        rhi::ComPtr<rhi::IShaderProgram> m_program{};
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

        // CPU-side DawnBringer-32 palette (normalized sRGB bytes), copied into
        // the per-draw RetroParameters constant for the selected scene slot.
        std::array<float4, kPaletteSize> m_palette;

        // Retired pipelines keyed by last-use fence value; reclaimed at every
        // render entry and drained unconditionally at shutdown/device-loss.
        DeferredReleaseQueue m_deferred_releases{};
        FenceCompletionValue m_last_submitted_value{kFenceValueNone};

        std::array<SceneTarget, kFrameRingSlots> m_scene_slots{};
        u32 m_scene_cursor{0u};
        u32 m_scene_slot{0u};

        bool m_scene_ready{false};
        bool m_scene_recorded{false};
        bool m_ready{false};
    };
}
