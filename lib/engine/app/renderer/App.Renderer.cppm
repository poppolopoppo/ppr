module;
export module engine.app:renderer;

import :renderer.types;
import :service.window;
import :window.handle;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // generic renderer: surfaces + queue + submission only
    // ------------------------------------------------------------------
    // Submission shapes (RenderView/DrawSubmission/ColorPassOptions/SceneView)
    // live in :renderer.types (Lane C); this class only owns surfaces,
    // the graphics queue, and encode/submit/present.

    class Renderer {
    public:
        struct SurfaceRecord {
            rhi::ComPtr<rhi::ISurface> m_surface;
            // NOTE: explicit zeros — mango's default Vector ctor leaves
            // components indeterminate.
            int2 m_size{0, 0};
            rhi::Format m_format{rhi::Format::Undefined};
            bool m_configured{false};
        };

        [[nodiscard]] rhi::Format getWindowSurfaceFormat(WindowHandle handle) const noexcept;

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service);

        [[nodiscard]] std::error_code createWindowSurface(IWindowService &window_service, const Window &window);

        [[nodiscard]] std::error_code destroyWindowSurface(WindowHandle handle);

        [[nodiscard]] std::error_code resizeWindowSurface(WindowHandle handle, int2 size);

        [[nodiscard]] std::error_code renderAndPresent(
            WindowHandle handle,
            std::span<const DrawSubmission> draws,
            const ColorPassOptions &options);

        /// Encodes + submits into an offscreen texture without presenting or
        /// waiting; the caller must waitForIdle() before readback.
        [[nodiscard]] std::error_code submitToTexture(
            rhi::ITexture &target,
            std::span<const DrawSubmission> draws,
            const ColorPassOptions &options);

        [[nodiscard]] std::error_code waitForIdle();

        [[nodiscard]] std::error_code shutdown();

    private:
        [[nodiscard]] std::error_code configureSurface_(SurfaceRecord &record, int2 size);

        [[nodiscard]] std::error_code submitToTarget_(
            rhi::ITexture &target,
            const ColorTargetInfo &target_info,
            std::span<const DrawSubmission> draws,
            const ColorPassOptions &options);

        [[nodiscard]] static std::error_code encodeDraws_(
            rhi::IRenderPassEncoder &pass,
            const ColorTargetInfo &target_info,
            std::span<const DrawSubmission> draws);

        // NOTE: temporary HWND-only adapter. A platform-neutral window-handle
        // adapter replaces this helper without touching call sites.
        [[nodiscard]] static rhi::WindowHandle toRhiWindowHandle_(void *const native) noexcept;

        FlatMap<WindowHandle, SurfaceRecord> m_surfaces{};
        rhi::ComPtr<rhi::ICommandQueue> m_queue;
        safe_ptr<IRhiService> m_rhi_service;
    };
}
