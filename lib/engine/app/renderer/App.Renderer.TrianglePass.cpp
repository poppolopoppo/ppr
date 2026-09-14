module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.triangle_pass;
import :renderer.types;
import :scene.camera;
import std;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(TrianglePass, info, none)

    namespace {
        struct DummyVertex {
            float position[3];
            float color[3];
        };

        constexpr DummyVertex kVertices[] = {
            {.position = {0.0f, 0.5f, 0.0f}, .color = {1.0f, 0.0f, 0.0f}},
            {.position = {0.5f, -0.5f, 0.0f}, .color = {0.0f, 1.0f, 0.0f}},
            {.position = {-0.5f, -0.5f, 0.0f}, .color = {0.0f, 0.0f, 1.0f}},
        };
    }

    namespace fs = std::filesystem;

    std::error_code TrianglePass::initialize(IRhiService &rhi_service, IShaderService &shader_service, const fs::path &content_dir) {
        rhi::IDevice &device = rhi_service.getDevice();

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, createInvariantRenderState_(device));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, createShaderProgram_(shader_service, device, content_dir));

        PPR_LOG(TrianglePass, info, "TrianglePass initialized");
        return default_value_v;
    }

    std::error_code TrianglePass::update([[maybe_unused]] TimeSpan dt, const CameraSnapshot &camera_view) {
        m_camera_view = camera_view;

        return default_value_v;
    }

    std::error_code TrianglePass::render(const DrawContext &draw_context) {
        if (not
            m_render_pipeline_key.has_value()
        or
        m_render_pipeline_key.value() != draw_context.m_render_pipeline_key)
        {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass, createRenderPipeline_(draw_context.m_device, draw_context.m_render_pipeline_key));
            m_render_pipeline_key = draw_context.m_render_pipeline_key;
        }

        rhi::ShaderCursor shader_cursor{};
        if (rhi::IShaderObject *const shader_object = draw_context.m_pass.bindPipeline(m_render_pipeline.get()); PPR_ENSURE(shader_object)) {
            shader_cursor = rhi::ShaderCursor(shader_object);
        } else {
            return make_error_code(std::errc::broken_pipe);
        }

        // Dereference the ConstantBuffer field so the data lands in the g_frame
        // sub-object's ordinary data buffer (which is what gets uploaded at draw time),
        // rather than in the root object's own buffer.
        rhi::ShaderCursor frame_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_frame"].getDereferenced(frame_cursor));

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, uploadFrameConstants_(frame_cursor));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .vertexBuffers = {{m_vertex_buffer.get(), 0u}},
            .vertexBufferCount = 1u,
        });

        draw_context.m_pass.draw({.vertexCount = 3u});

        return default_value_v;
    }

    std::error_code TrianglePass::shutdown() {
        PPR_LOG(TrianglePass, info, "TrianglePass shut down", {
            {"has_pipeline", m_render_pipeline != nullptr},
        });

        m_render_pipeline_key.reset();
        m_render_pipeline.setNull();
        m_shader_program.setNull();
        m_vertex_layout.setNull();
        m_vertex_buffer.setNull();
        return default_value_v;
    }

    std::error_code TrianglePass::createInvariantRenderState_(rhi::IDevice &device) {
        constexpr rhi::InputElementDesc elements[] = {
            {"POSITION", 0, rhi::Format::RGB32Float, PPR_OFFSETOF(DummyVertex, position), 0},
            {"COLOR", 0, rhi::Format::RGB32Float, PPR_OFFSETOF(DummyVertex, color), 0},
        };

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.createInputLayout(
            safe_narrowing(sizeof(DummyVertex)),
            elements,
            2u,
            m_vertex_layout.writeRef()));

        rhi::BufferDesc vb_desc{};
        vb_desc.size = sizeof(kVertices);
        vb_desc.usage = rhi::BufferUsage::VertexBuffer;
        vb_desc.defaultState = rhi::ResourceState::VertexBuffer;
        vb_desc.memoryType = rhi::MemoryType::DeviceLocal;
        vb_desc.label = "triangle vertex buffer";

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.createBuffer(vb_desc, kVertices, m_vertex_buffer.writeRef()));

        return default_value_v;
    }

    std::error_code TrianglePass::createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule triangle_shader{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_service.loadModuleFromFile(
            content_dir / TEXT("shaders") / TEXT("triangle.slang"),
            "triangle",
            triangle_shader.writeRef()));

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = triangle_shader.get();
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

        return default_value_v;
    }

    std::error_code TrianglePass::createRenderPipeline_(rhi::IDevice &device, const RenderPipelineSignature &signature) {
        PPR_LOG(TrianglePass, info, "rebuilding triangle pipeline", {
            {"samples", signature.m_sample_count},
        });

        if (signature.m_color_formats.size() != 1u ||
            signature.m_depth_stencil_format.has_value() ||
            signature.m_sample_count != 1u) {
            PPR_LOG(TrianglePass, error, "unsupported render pipeline signature", {
                {"color_format_count", signature.m_color_formats.size()},
                {"has_depth_stencil", signature.m_depth_stencil_format.has_value()},
                {"sample_count", signature.m_sample_count},
            });
            return make_error_code(std::errc::operation_not_supported);
        }

        rhi::ColorTargetDesc color_target{};
        color_target.format = signature.m_color_formats.front();
        color_target.enableBlend = false;

        rhi::RenderPipelineDesc pipeline_desc{};
        pipeline_desc.program = m_shader_program.get();
        pipeline_desc.inputLayout = m_vertex_layout.get();
        pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        pipeline_desc.targets = &color_target;
        pipeline_desc.targetCount = 1u;
        pipeline_desc.multisample.sampleCount = signature.m_sample_count;
        pipeline_desc.label = "triangle pipeline";

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.createRenderPipeline(pipeline_desc, m_render_pipeline.writeRef()));

        return default_value_v;
    }

    std::error_code TrianglePass::uploadFrameConstants_(rhi::ShaderCursor &frame_cursor) {
        FrameConstants frame{};
        frame.m_view = m_camera_view.m_view;
        frame.m_projection = m_camera_view.m_projection;
        frame.m_view_projection = m_camera_view.m_view_projection;
        frame.m_inverse_view_projection = m_camera_view.m_invert_view_projection;
        frame.m_camera_position = float4{m_camera_view.m_origin, 1.0f}; // point promotion (w == 1).
        frame.m_viewport_size = float4{m_camera_view.m_viewport_size, 0.0f, 0.0f};

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, frame_cursor.setData(&frame, sizeof(FrameConstants)));

        return default_value_v;
    }
}
