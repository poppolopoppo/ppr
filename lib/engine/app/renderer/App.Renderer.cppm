module;
#include "pP/Macros.h"
export module engine.app:renderer;

import :service.window;
import :viewport;
import :camera;
import engine.math;
import engine.core;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    class Renderer {
    public:
        using OverlayCallback = std23::function_ref<std::error_code (rhi::IRenderPassEncoder &pass)>;
        using ViewportEntry = pP::ViewportEntry;

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service,
            IWindowService &window_service,
            const Window &window,
            const std::filesystem::path &content_dir = {});

        [[nodiscard]] rhi::Format getSurfaceFormat() const noexcept;

        [[nodiscard]] std::error_code render(std::optional<OverlayCallback> overlay);

        [[nodiscard]] std::error_code render(std::span<const ViewportEntry> viewports);

        [[nodiscard]] std::error_code drawTriangle(
            rhi::IRenderPassEncoder &pass,
            const rhi::Viewport &viewport,
            const rhi::ScissorRect &scissor);

        [[nodiscard]] std::error_code drawTriangle(
            rhi::IRenderPassEncoder &pass,
            const Camera &camera,
            const rhi::Viewport &viewport,
            const rhi::ScissorRect &scissor);

        /// Renders the given viewports into an arbitrary color target (offscreen readback, etc.).
        /// Waits for GPU completion before returning so the target can be read back.
        [[nodiscard]] std::error_code renderInto(rhi::ITexture &target, std::span<const ViewportEntry> viewports);

        [[nodiscard]] std::error_code onResize(int2 new_size);

        [[nodiscard]] std::error_code shutdown();

    private:
        [[nodiscard]] std::error_code rebuildPipeline_(rhi::IDevice &device);

        /// Creates a persistent root shader object and caches the g_frame cursor
        /// resolved from it; must be re-run whenever the pipeline is rebuilt.
        [[nodiscard]] std::error_code resolveFrameCursor_(rhi::IDevice &device);

        [[nodiscard]] std::error_code createSurface_(IWindowService &window_service, const Window &window);

        /// Encodes and submits the viewports into a color target without waiting;
        /// the caller is responsible for synchronization (present or explicit wait).
        [[nodiscard]] std::error_code renderInto_(rhi::ITexture &target, std::span<const ViewportEntry> viewports);

        rhi::ComPtr<rhi::ICommandQueue> m_queue;
        rhi::ComPtr<rhi::ISurface> m_surface;
        rhi::ComPtr<rhi::IRenderPipeline> m_pipeline;
        rhi::ComPtr<rhi::IBuffer> m_vertex_buffer;
        rhi::ComPtr<rhi::IInputLayout> m_input_layout;
        rhi::ComPtr<rhi::IShaderProgram> m_program;
        rhi::ComPtr<rhi::IShaderObject> m_root_object;
        rhi::ShaderCursor m_frame_cursor;

        shader::SharedModule m_triangle_shader;

        rhi::Format m_surface_format{rhi::Format::Undefined};

        int2 m_framebuffer_size{};
        rhi::DeviceType m_device_type{rhi::DeviceType::Default};
        safe_ptr<IRhiService> m_rhi_service;

        struct FrameConstants {
            float4x4 m_view = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_view_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_inverse_view_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4 m_camera_position = float4{0, 0, 0, 0};
            float4 m_camera_velocity = float4{0, 0, 0, 0};
            float4 m_viewport_size = float4{0, 0, 0, 0};
        };
        static_assert(sizeof(FrameConstants) == 304, "FrameConstants must match the HLSL layout (4 * float4x4 + 3 * float4)");

        const Camera *m_last_camera{nullptr};
        u64 m_last_camera_version{0};
    };
}
