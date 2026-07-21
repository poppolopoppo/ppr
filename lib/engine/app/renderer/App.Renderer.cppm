module;
#include "pP/Macros.h"
export module engine.app:renderer;

import :service.window;
import engine.core;
import engine.rhi;
import std;

export namespace pP {
    class Renderer {
    public:
        using OverlayCallback = std23::function_ref<std::error_code (rhi::IRenderPassEncoder &pass)>;

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service,
            IWindowService &window_service,
            const Window &window,
            const std::filesystem::path &content_dir = {});

        [[nodiscard]] rhi::Format getSurfaceFormat() const noexcept;

        [[nodiscard]] std::error_code render(std::optional<OverlayCallback> overlay);

        [[nodiscard]] std::error_code onResize(int2 new_size);

        [[nodiscard]] std::error_code shutdown();

    private:
        rhi::ComPtr<rhi::ICommandQueue> m_queue;
        rhi::ComPtr<rhi::ISurface> m_surface;
        rhi::ComPtr<rhi::IRenderPipeline> m_pipeline;
        rhi::ComPtr<rhi::IBuffer> m_vertex_buffer;
        rhi::ComPtr<rhi::IInputLayout> m_input_layout;
        rhi::ComPtr<rhi::IShaderProgram> m_program;

        rhi::Format m_surface_format{rhi::Format::Undefined};

        int2 m_framebuffer_size{};
    };
}
