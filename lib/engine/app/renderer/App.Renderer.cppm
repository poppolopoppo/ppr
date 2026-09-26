module;
export module engine.app:renderer;

import :renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    class Window;
    using WindowHandle = Numeric<void *, Window>;

    // ------------------------------------------------------------------
    // generic renderer: surfaces + queue + submission only
    // ------------------------------------------------------------------

    class Renderer final {
    public:
        rhi::Format m_preferred_surface_format{rhi::Format::Undefined};
        u32 m_desired_image_count{3u};
        bool m_enable_vsync{true};

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service);

        [[nodiscard]] std::error_code shutdown();

        [[nodiscard]] std::error_code render(
            string_literal description,
            const rhi::RenderPassDesc &render_pass,
            std::initializer_list<DrawSubmission> draws);

        [[nodiscard]] std::error_code renderToTexture(
            rhi::ITexture &render_target,
            std::initializer_list<DrawSubmission> draws,
            const ColorAttachmentOps &options = {});

        [[nodiscard]] std::error_code renderAndPresent(
            const Window &window,
            std::initializer_list<SurfaceRenderPass> passes);

        [[nodiscard]] std::error_code waitOnHost();

        [[nodiscard]] std::error_code destroyWindowSurface(const Window &window);

    private:
        // Per-surface cache of a pass-shape signature, one slot per cacheable
        // depth policy: slot 0 = none, slot 1 = renderer_owned. The 3D pass
        // (renderer_owned depth) and the UI pass (no depth) are different
        // shapes, so each policy owns its entry — one signature is never stored
        // across mixed passes. External depth and any pass carrying extra color
        // attachments is caller-shaped and is never cached.
        //
        // Each entry holds the signature inputs ONLY — (color formats, sample
        // count, depth format) — plus the extent it was computed for. Future
        // per-frame jitter must NEVER enter this signature: jitter lives in
        // frame constants (TSR direction), never in the pipeline key, or every
        // frame would compare unequal and wipe the cached variants.
        //
        // Those inputs derive from the surface config and the depth texture, so
        // resizeWindowSurface_ is the only place that recomputes them; it clears
        // both slots before reconfiguring and reseeds them on success, and the
        // stored extent lets a per-frame hit be rejected outright when the
        // surface no longer matches.
        struct SurfaceSignatureCache final {
            ESurfaceDepthPolicy m_depth_policy{ESurfaceDepthPolicy::none};
            RenderPipelineSignature m_signature{};
            int2 m_extent{zero_v};
            bool m_valid{false};
        };

        struct SurfaceRecord final {
            rhi::ComPtr<rhi::ISurface> m_surface{};
            rhi::ComPtr<rhi::ITexture> m_depth_texture{};
            rhi::ComPtr<rhi::ITextureView> m_depth_view{};
            int2 m_extent{zero_v};
            std::array<SurfaceSignatureCache, 2u> m_signatures{};
            bool m_configured: 1 {false};
        };

        // Surface-hoisted signature resolution for one phase-1 pass. The
        // attachments are still validated on every frame — the fail-closed
        // kMaxColorFormats check stays on the per-frame path, because the cache
        // holds the signature VALUE and never a "validated" verdict — while a
        // steady-state frame reuses the entry resizeWindowSurface_ recomputed
        // for the current surface shape.
        [[nodiscard]] std::error_code surfaceSignature_(
            SurfaceRecord &record,
            const SurfaceRenderPass &surface_pass,
            const rhi::RenderPassDesc &render_pass,
            RenderPipelineSignature &out_signature,
            int2 &out_target_extent);

        // Reseeds every cacheable per-pass signature entry from the inputs that
        // define it: the configured swapchain format, the backbuffer sample
        // count, and the depth texture's format. Called only from
        // resizeWindowSurface_ — the one place the surface shape changes.
        void recomputeSurfaceSignatures_(SurfaceRecord &record);

        [[nodiscard]] std::error_code createWindowSurface_(const Window &window, SurfaceRecord **out_record);

        [[nodiscard]] std::error_code resizeWindowSurface_(SurfaceRecord &record, const int2 &new_extent);

        [[nodiscard]] std::error_code createSurfaceDepth_(const int2 &extent, rhi::ComPtr<rhi::ITexture> &out_texture,
                                                          rhi::ComPtr<rhi::ITextureView> &out_view) const;

        [[nodiscard]] std::error_code renderDraws_(string_literal description, const rhi::RenderPassDesc &render_pass,
                                                   std::span<const DrawSubmission> draws);

        [[nodiscard]] std::error_code destroyWindowSurface_(WindowHandle window_handle);

        [[nodiscard]] bool onRenderThread_() const noexcept;

        FlatMap<WindowHandle, SurfaceRecord> m_surfaces{};
        rhi::ComPtr<rhi::ICommandQueue> m_graphics_queue{};
        safe_ptr<IRhiService> m_rhi_service{};
        std::thread::id m_owner{};
    };
}
