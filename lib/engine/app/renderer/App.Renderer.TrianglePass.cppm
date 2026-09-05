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
            float4 m_camera_position = float4{0, 0, 0, 0};
            float4 m_viewport_size = float4{0, 0, 0, 0};
        };

        static_assert(sizeof(FrameConstants) == 288, "FrameConstants must match the HLSL layout (4 * float4x4 + 2 * float4)");

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code draw(
            rhi::IRenderPassEncoder &pass,
            const SceneView &scene_view,
            const ColorTargetInfo &target_info);

        [[nodiscard]] std::error_code shutdown();

    private:
        [[nodiscard]] std::error_code ensurePipeline_(const ColorTargetInfo &target_info);

        [[nodiscard]] std::error_code rebuildPipeline_(rhi::IDevice &device, const ColorTargetInfo &target_info);

        /// Creates a persistent root shader object and caches the g_frame cursor
        /// resolved from it; must be re-run whenever the pipeline is rebuilt.
        [[nodiscard]] std::error_code resolveFrameCursor_(rhi::IDevice &device);

        [[nodiscard]] std::error_code uploadFrameConstants_(const CameraSnapshot &snapshot);

        rhi::ComPtr<rhi::IRenderPipeline> m_pipeline;
        rhi::ComPtr<rhi::IBuffer> m_vertex_buffer;
        rhi::ComPtr<rhi::IInputLayout> m_input_layout;
        rhi::ComPtr<rhi::IShaderProgram> m_program;
        rhi::ComPtr<rhi::IShaderObject> m_root_object;
        rhi::ShaderCursor m_frame_cursor;

        shader::SharedModule m_triangle_shader;

        rhi::Format m_pipeline_format{rhi::Format::Undefined};
        u32 m_pipeline_sample_count{1};
        std::size_t m_last_revision{std::numeric_limits<std::size_t>::max()};
        float2 m_last_viewport_size{zero_v};
        safe_ptr<IRhiService> m_rhi_service;
    };
}
