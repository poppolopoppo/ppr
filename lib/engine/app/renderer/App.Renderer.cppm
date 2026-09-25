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
        struct SurfaceRecord final {
            rhi::ComPtr<rhi::ISurface> m_surface{};
            rhi::ComPtr<rhi::ITexture> m_depth_texture{};
            rhi::ComPtr<rhi::ITextureView> m_depth_view{};
            int2 m_extent{zero_v};
            bool m_configured: 1 {false};
        };

        [[nodiscard]] std::error_code createWindowSurface_(const Window &window, SurfaceRecord **out_record);

        [[nodiscard]] std::error_code resizeWindowSurface_(SurfaceRecord &record, const int2 &new_extent);

        [[nodiscard]] std::error_code createSurfaceDepth_(const int2 &extent, rhi::ComPtr<rhi::ITexture> &out_texture,
                                                          rhi::ComPtr<rhi::ITextureView> &out_view) const;

        [[nodiscard]] std::error_code renderDraws_(string_literal description, const rhi::RenderPassDesc &render_pass,
                                                   std::span<const DrawSubmission> draws);

        [[nodiscard]] std::error_code destroyWindowSurface_(WindowHandle window_handle);

        FlatMap<WindowHandle, SurfaceRecord> m_surfaces{};
        rhi::ComPtr<rhi::ICommandQueue> m_graphics_queue{};
        safe_ptr<IRhiService> m_rhi_service{};
    };
}
