module;
export module engine.app:renderer.triangle_pass;

import :renderer.types;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // triangle pass: owns the triangle shader/pipeline/resources
    // ------------------------------------------------------------------
    // The Renderer stays content-free; scene content lives here. Viewport
    // geometry always derives from SceneView::m_render_view, camera data
    // always from the snapshot — mutable Camera is never consulted.

    class TrianglePass {
    public:
        struct FrameConstants {
            float4x4 m_view = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_view_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_inverse_view_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            // Zero default; uploadFrameConstants_ promotes origin to a point via float4{origin, 1}.
            float4 m_camera_position = float4{0, 0, 0, 0};
            float4 m_viewport_size = float4{0, 0, 0, 0};
        };

        static_assert(sizeof(FrameConstants) == 288, "FrameConstants must match the HLSL layout (4 * float4x4 + 2 * float4)");

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code update(TimeSpan dt, const CameraSnapshot &camera_view);

        [[nodiscard]] std::error_code render(const DrawContext &draw_context);

        [[nodiscard]] std::error_code shutdown();

    private:
        std::error_code createInvariantRenderState_(rhi::IDevice &device);

        std::error_code createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code createRenderPipeline_(rhi::IDevice &device, const RenderPipelineSignature &signature);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor);

        rhi::ComPtr<rhi::IBuffer> m_vertex_buffer{};
        rhi::ComPtr<rhi::IInputLayout> m_vertex_layout{};
        rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};

        rhi::ComPtr<rhi::IRenderPipeline> m_render_pipeline{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;

        CameraSnapshot m_camera_view;
    };
}
