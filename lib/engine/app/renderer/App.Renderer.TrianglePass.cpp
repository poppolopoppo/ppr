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
    PPR_DEFINE_LOG_CATEGORY(TrianglePass, info, none)

    namespace {
        struct DummyVertex {
            float position[3];
            float color[3];
        };

        constexpr DummyVertex kVertices[] = {
            {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
            {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        };
    }

    namespace fs = std::filesystem;

    std::error_code TrianglePass::initialize(IRhiService &rhi_service, const fs::path &content_dir) {
        m_rhi_service = safe_ptr{&rhi_service};
        rhi::IDevice &device = rhi_service.getDevice();

        {
            const safe_ptr<IShaderService> shader_service = IShaderService::get();
            PPR_ASSERT(shader_service.isValid());

            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                shader_service->loadModuleFromFile(
                    content_dir / TEXT("shaders") / TEXT("triangle.slang"),
                    "triangle",
                    m_triangle_shader.writeRef()));

            PPR_ASSERT(m_triangle_shader);
        }

        {
            slang::IModule *module = m_triangle_shader.get();

            shader::ComPtr<slang::IEntryPoint> vertex_ep;
            RHI_RETURN_ERROR_ON_FAIL(TrianglePass, module->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

            shader::ComPtr<slang::IEntryPoint> fragment_ep;
            RHI_RETURN_ERROR_ON_FAIL(TrianglePass, module->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

            slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

            rhi::ShaderProgramDesc program_desc{};
            program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_desc.slangGlobalScope = module;
            program_desc.slangEntryPoints = entry_points;
            program_desc.slangEntryPointCount = 2u;

            shader::Diagnose diagnostics;
            RHI_RETURN_ERROR_ON_FAIL(TrianglePass, device.createShaderProgram(program_desc, m_program.writeRef(), diagnostics.writeRef()));
        }

        {
            rhi::InputElementDesc elements[] = {
                {"POSITION", 0, rhi::Format::RGB32Float, PPR_OFFSETOF(DummyVertex, position), 0},
                {"COLOR", 0, rhi::Format::RGB32Float, PPR_OFFSETOF(DummyVertex, color), 0},
            };

            RHI_RETURN_ERROR_ON_FAIL(
                TrianglePass,
                device.createInputLayout(
                    safe_narrowing(sizeof(DummyVertex)),
                    elements,
                    2u,
                    m_input_layout.writeRef()));
        }

        {
            rhi::BufferDesc vb_desc{};
            vb_desc.size = sizeof(kVertices);
            vb_desc.usage = rhi::BufferUsage::VertexBuffer;
            vb_desc.defaultState = rhi::ResourceState::VertexBuffer;
            vb_desc.memoryType = rhi::MemoryType::DeviceLocal;
            vb_desc.label = "triangle vertex buffer";

            RHI_RETURN_ERROR_ON_FAIL(TrianglePass, device.createBuffer(vb_desc, kVertices, m_vertex_buffer.writeRef()));
        }

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, resolveFrameCursor_(device));

        PPR_LOG(TrianglePass, info, "TrianglePass initialized", {
            {"has_program", m_program != nullptr},
        });
        return default_value_v;
    }

    std::error_code TrianglePass::draw(
        rhi::IRenderPassEncoder &pass,
        const SceneView &scene_view,
        const ColorTargetInfo &target_info) {
        PPR_ASSERT(m_rhi_service.isValid());
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, ensurePipeline_(target_info));

        PPR_ASSERT(m_pipeline);
        PPR_ASSERT(m_vertex_buffer);
        PPR_ASSERT(m_root_object);
        PPR_ASSERT(m_frame_cursor.isValid());

        pass.insertDebugMarker("drawTriangle", rhi::MarkerColor{1, 0, 0});

        pass.bindPipeline(m_pipeline.get(), m_root_object.get());
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, uploadFrameConstants_(scene_view.m_camera));

        // Ownership split: Renderer::encodeDraws_ owns viewport/scissor per
        // submission; this pass owns pipeline, frame constants, vertex-buffer
        // binding, and the draw itself.
        const RenderView &view = scene_view.m_render_view;
        const rhi::BufferOffsetPair vertex_buffer{m_vertex_buffer.get(), 0};
        pass.setRenderState({
            .viewports = {view.m_viewport},
            .viewportCount = 1,
            .scissorRects = {view.m_scissor},
            .scissorRectCount = 1,
            .vertexBuffers = {vertex_buffer},
            .vertexBufferCount = 1,
        });

        pass.draw({.vertexCount = 3});
        return default_value_v;
    }

    std::error_code TrianglePass::shutdown() {
        PPR_LOG(TrianglePass, info, "TrianglePass shut down", {
            {"has_pipeline", m_pipeline != nullptr},
        });

        m_pipeline.setNull();
        m_vertex_buffer.setNull();
        m_input_layout.setNull();
        m_program.setNull();
        m_root_object.setNull();
        m_triangle_shader = shader::SharedModule{};
        m_pipeline_format = rhi::Format::Undefined;
        m_pipeline_sample_count = 1;
        m_rhi_service.reset();
        return default_value_v;
    }

    std::error_code TrianglePass::ensurePipeline_(const ColorTargetInfo &target_info) {
        PPR_ASSERT(m_rhi_service.isValid());
        if (m_pipeline && m_pipeline_format == target_info.m_format
            && m_pipeline_sample_count == target_info.m_sample_count) {
            return default_value_v;
        }
        return rebuildPipeline_(m_rhi_service->getDevice(), target_info);
    }

    std::error_code TrianglePass::rebuildPipeline_(rhi::IDevice &device, const ColorTargetInfo &target_info) {
        PPR_LOG(TrianglePass, info, "rebuilding triangle pipeline", {
            {"samples", target_info.m_sample_count},
        });

        slang::IModule *module = m_triangle_shader.get();

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        RHI_RETURN_ERROR_ON_FAIL(TrianglePass, module->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        RHI_RETURN_ERROR_ON_FAIL(TrianglePass, module->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = module;
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::ComPtr<rhi::IShaderProgram> new_program;
        {
            shader::Diagnose diagnostics;
            RHI_RETURN_ERROR_ON_FAIL(TrianglePass, device.createShaderProgram(program_desc, new_program.writeRef(), diagnostics.writeRef()));
        }

        rhi::ColorTargetDesc color_target{};
        color_target.format = target_info.m_format;
        color_target.enableBlend = false;

        rhi::RenderPipelineDesc pipeline_desc{};
        pipeline_desc.program = new_program.get();
        pipeline_desc.inputLayout = m_input_layout.get();
        pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        pipeline_desc.targets = &color_target;
        pipeline_desc.targetCount = 1;
        pipeline_desc.multisample.sampleCount = target_info.m_sample_count;
        pipeline_desc.label = "triangle pipeline";

        RHI_RETURN_ERROR_ON_FAIL(TrianglePass, device.createRenderPipeline(pipeline_desc, m_pipeline.writeRef()));

        m_program = std::move(new_program);
        m_pipeline_format = target_info.m_format;
        m_pipeline_sample_count = target_info.m_sample_count;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, resolveFrameCursor_(device));
        return default_value_v;
    }

    std::error_code TrianglePass::resolveFrameCursor_(rhi::IDevice &device) {
        RHI_RETURN_ERROR_ON_FAIL(TrianglePass, device.createRootShaderObject(m_program.get(), m_root_object.writeRef()));

        // Dereference the ConstantBuffer field so the data lands in the g_frame
        // sub-object's ordinary data buffer (which is what gets uploaded at draw time),
        // rather than in the root object's own buffer.
        rhi::ShaderCursor root_cursor{m_root_object.get()};
        m_frame_cursor = root_cursor["g_frame"].getDereferenced();
        PPR_ASSERT(m_frame_cursor.isValid());
        return default_value_v;
    }

    std::error_code TrianglePass::uploadFrameConstants_(const CameraSnapshot &snapshot) {
        FrameConstants frame{};
        frame.m_view = snapshot.m_view;
        frame.m_projection = snapshot.m_projection;
        frame.m_view_projection = snapshot.m_view_projection;
        frame.m_inverse_view_projection = snapshot.m_invert_view_projection;
        frame.m_camera_position = float4{snapshot.m_origin, 1.0f}; // point promotion (w == 1).
        frame.m_viewport_size = float4{snapshot.m_viewport_size, 0.0f, 0.0f};

        RHI_RETURN_ERROR_ON_FAIL(TrianglePass, m_frame_cursor.setData(&frame, sizeof(FrameConstants)));
        return default_value_v;
    }
}
