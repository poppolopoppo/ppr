module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer;
import :service.window;
import :window.handle;
import std;
import engine.core;
import engine.rhi;
import engine.shader;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Renderer, info, none)

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

    rhi::Format Renderer::getSurfaceFormat() const noexcept {
        return m_surface_format;
    }

    std::error_code Renderer::initialize(
        IRhiService &rhi_service,
        IWindowService &window_service,
        const Window &window,
        const fs::path &content_dir) {
        m_framebuffer_size = window.m_framebuffer_size;
        m_device_type = rhi_service.getDevice().getDeviceType();
        m_rhi_service = safe_ptr{&rhi_service};

        rhi::IDevice &device = rhi_service.getDevice();

        // Get graphics queue
        RHI_RETURN_ERROR_ON_FAIL(Renderer, device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef()));

        // Create surface from native window handle
        PPR_RETURN_ERROR_ON_FAIL(Renderer, createSurface_(window_service, window));

        // Configure swap chain
        {
            rhi::SurfaceConfig surface_config{};
            surface_config.width = static_cast<u32>(m_framebuffer_size.x);
            surface_config.height = static_cast<u32>(m_framebuffer_size.y);
            surface_config.desiredImageCount = 3;
            surface_config.vsync = true;

            RHI_RETURN_ERROR_ON_FAIL(Renderer, m_surface->configure(surface_config));

            m_surface_format = m_surface->getInfo().preferredFormat;
        }

        // Load shaders via IShaderService
        {
            const safe_ptr<IShaderService> shader_service = IShaderService::get();
            PPR_ASSERT(shader_service.isValid());

            PPR_RETURN_ERROR_ON_FAIL(Renderer,
                shader_service->loadModuleFromFile(
                    content_dir / TEXT("shaders") / TEXT("triangle.slang"),
                    "triangle",
                    m_triangle_shader.writeRef()));

            PPR_ASSERT(m_triangle_shader);
        }

        // Create shader program
        {
            slang::IModule *module = m_triangle_shader.get();

            shader::ComPtr<slang::IEntryPoint> vertex_ep;
            RHI_RETURN_ERROR_ON_FAIL(Renderer, module->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

            shader::ComPtr<slang::IEntryPoint> fragment_ep;
            RHI_RETURN_ERROR_ON_FAIL(Renderer, module->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

            slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

            rhi::ShaderProgramDesc program_desc{};
            program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_desc.slangGlobalScope = module;
            program_desc.slangEntryPoints = entry_points;
            program_desc.slangEntryPointCount = 2u;

            shader::Diagnose diagnostics;
            RHI_RETURN_ERROR_ON_FAIL(Renderer, device.createShaderProgram(program_desc, m_program.writeRef(), diagnostics.writeRef()));
        }

        // Create input layout
        {
            rhi::InputElementDesc elements[] = {
                {"POSITION", 0, rhi::Format::RGB32Float, PPR_OFFSETOF(DummyVertex, position), 0},
                {"COLOR", 0, rhi::Format::RGB32Float, PPR_OFFSETOF(DummyVertex, color), 0},
            };

            RHI_RETURN_ERROR_ON_FAIL(
                Renderer,
                device.createInputLayout(
                    safe_narrowing(sizeof(DummyVertex)),
                    elements,
                    2u,
                    m_input_layout.writeRef()));
        }

        // Create vertex buffer
        {
            rhi::BufferDesc vb_desc{};
            vb_desc.size = sizeof(kVertices);
            vb_desc.usage = rhi::BufferUsage::VertexBuffer;
            vb_desc.defaultState = rhi::ResourceState::VertexBuffer;
            vb_desc.memoryType = rhi::MemoryType::DeviceLocal;
            vb_desc.label = "triangle vertex buffer";

            RHI_RETURN_ERROR_ON_FAIL(Renderer, device.createBuffer(vb_desc, kVertices, m_vertex_buffer.writeRef()));
        }

        // Create render pipeline
        {
            const rhi::SurfaceInfo &surface_info = m_surface->getInfo();

            rhi::ColorTargetDesc color_target{};
            color_target.format = surface_info.preferredFormat;
            color_target.enableBlend = false;

            rhi::RenderPipelineDesc pipeline_desc{};
            pipeline_desc.program = m_program.get();
            pipeline_desc.inputLayout = m_input_layout.get();
            pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            pipeline_desc.targets = &color_target;
            pipeline_desc.targetCount = 1;
            pipeline_desc.label = "triangle pipeline";

            RHI_RETURN_ERROR_ON_FAIL(Renderer, device.createRenderPipeline(pipeline_desc, m_pipeline.writeRef()));
        }

        PPR_LOG(Renderer, info, "Renderer initialized successfully", {
                {"width", m_framebuffer_size.x},
                {"height", m_framebuffer_size.y},
                });

        return default_value_v;
    }

    std::error_code Renderer::rebuildPipeline_(rhi::IDevice &device) {
        PPR_LOG(Renderer, info, "shader hot-reloaded, rebuilding pipeline");

        slang::IModule *module = m_triangle_shader.get();

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        RHI_RETURN_ERROR_ON_FAIL(Renderer, module->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        RHI_RETURN_ERROR_ON_FAIL(Renderer, module->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = module;
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::ComPtr<rhi::IShaderProgram> new_program;
        {
            shader::Diagnose diagnostics;
            RHI_RETURN_ERROR_ON_FAIL(Renderer, device.createShaderProgram(program_desc, new_program.writeRef(), diagnostics.writeRef()));
        }

        const rhi::SurfaceInfo &surface_info = m_surface->getInfo();

        rhi::ColorTargetDesc color_target{};
        color_target.format = surface_info.preferredFormat;
        color_target.enableBlend = false;

        rhi::RenderPipelineDesc pipeline_desc{};
        pipeline_desc.program = new_program.get();
        pipeline_desc.inputLayout = m_input_layout.get();
        pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        pipeline_desc.targets = &color_target;
        pipeline_desc.targetCount = 1;
        pipeline_desc.label = "triangle pipeline";

        RHI_RETURN_ERROR_ON_FAIL(Renderer, device.createRenderPipeline(pipeline_desc, m_pipeline.writeRef()));

        m_program = std::move(new_program);
        return default_value_v;
    }

    std::error_code Renderer::onResize(int2 new_size) {
        PPR_ASSERT(m_surface);
        if (new_size.x <= 0 || new_size.y <= 0) {
            if (m_surface) [[likely]] {
                RHI_RETURN_ERROR_ON_FAIL(Renderer, m_surface->unconfigure());
            }
            return default_value_v;
        }

        m_framebuffer_size = new_size;
        RHI_RETURN_ERROR_ON_FAIL(Renderer, m_queue->waitOnHost());

        rhi::SurfaceConfig surface_config{};
        surface_config.width = static_cast<u32>(new_size.x);
        surface_config.height = static_cast<u32>(new_size.y);
        surface_config.desiredImageCount = 3;
        surface_config.vsync = true;

        RHI_RETURN_ERROR_ON_FAIL(Renderer, m_surface->configure(surface_config));

        PPR_LOG(Renderer, info, "surface resized", {
            {"width", new_size.x},
            {"height", new_size.y},
        });

        return default_value_v;
    }

std::error_code Renderer::shutdown() {
    PPR_LOG(Renderer, info, "Renderer shut down");

    PPR_DEFER {
        m_pipeline.setNull();
        m_vertex_buffer.setNull();
        m_input_layout.setNull();
        m_program.setNull();
        m_surface.setNull();
        m_queue.setNull();
    };

    if (m_queue) {
        RHI_RETURN_ERROR_ON_FAIL(Renderer, m_queue->waitOnHost());
    }

    if (m_surface) {
        RHI_RETURN_ERROR_ON_FAIL(Renderer, m_surface->unconfigure());
    }

    return default_value_v;
}

std::error_code Renderer::createSurface_(IWindowService &window_service, const Window &window) {
    void *const native = window_service.getNativeHandle(window);
    if (native == nullptr) [[unlikely]] {
        return std::make_error_code(std::errc::invalid_argument);
    }

    const auto wh = rhi::WindowHandle::fromHwnd(native);
    RHI_RETURN_ERROR_ON_FAIL(Renderer, m_rhi_service->getDevice().createSurface(wh, m_surface.writeRef()));

    rhi::SurfaceConfig surface_config{};
    surface_config.width = static_cast<u32>(m_framebuffer_size.x);
    surface_config.height = static_cast<u32>(m_framebuffer_size.y);
    surface_config.desiredImageCount = 3;
    surface_config.vsync = true;

    RHI_RETURN_ERROR_ON_FAIL(Renderer, m_surface->configure(surface_config));

    m_surface_format = m_surface->getInfo().preferredFormat;
    return default_value_v;
}

std::error_code Renderer::render(const std::optional<OverlayCallback> overlay) {
    if (overlay) {
        const ViewportEntry entry{
            .viewport = rhi::Viewport{
                0.0f, 0.0f,
                static_cast<float>(m_framebuffer_size.x),
                static_cast<float>(m_framebuffer_size.y),
                0.0f, 1.0f},
            .scissor = rhi::ScissorRect{
                0, 0,
                static_cast<uint32_t>(m_framebuffer_size.x),
                static_cast<uint32_t>(m_framebuffer_size.y)},
            .draw = *overlay};
        return render(std::span{&entry, 1});
    }
    return render(std::span<const ViewportEntry>{});
}

std::error_code Renderer::render(const std::span<const ViewportEntry> viewports) {
    PPR_ASSERT(m_surface);
    PPR_ASSERT(m_queue);

#if 0
    // Hot-reload check: rebuild pipeline if shader was recompiled
    if (m_triangle_shader.wasReloaded()) {
        rhi::IDevice &device = IRhiService::get()->getDevice();
        PPR_RETURN_ERROR_ON_FAIL(Renderer, rebuildPipeline_(device));
    }
#endif

    // Acquire next back-buffer image
    rhi::ComPtr<rhi::ITexture> image;
    RHI_RETURN_ERROR_ON_FAIL(Renderer, m_surface->acquireNextImage(image.writeRef()));

    // Create command encoder
    rhi::ComPtr<rhi::ICommandEncoder> encoder;
    RHI_RETURN_ERROR_ON_FAIL(Renderer, m_queue->createCommandEncoder(encoder.writeRef()));

    // Begin render pass
    rhi::RenderPassColorAttachment color_attachment{};
    color_attachment.view = image->getDefaultView();
    color_attachment.loadOp = rhi::LoadOp::Clear;
    color_attachment.clearValue[0] = 0.1f;
    color_attachment.clearValue[1] = 0.1f;
    color_attachment.clearValue[2] = 0.2f;
    color_attachment.clearValue[3] = 1.0f;

    rhi::RenderPassDesc render_pass_desc{};
    render_pass_desc.colorAttachments = &color_attachment;
    render_pass_desc.colorAttachmentCount = 1;

    rhi::IRenderPassEncoder *const pass = encoder->beginRenderPass(render_pass_desc);
    PPR_ASSERT(pass != nullptr);

    // Render each viewport
    for (const auto &entry : viewports) {
        if (entry.pipeline) {
            pass->bindPipeline(entry.pipeline.get());
        }

        pass->setRenderState({
            .viewports = {entry.viewport},
            .viewportCount = 1,
            .scissorRects = {entry.scissor},
            .scissorRectCount = 1,
        });

        PPR_RETURN_ERROR_ON_FAIL(Renderer, entry.draw(*pass));
    }

    // End pass and finish encoder
    pass->end();

    rhi::ComPtr<rhi::ICommandBuffer> cmd_buffer;
    RHI_RETURN_ERROR_ON_FAIL(Renderer, encoder->finish(cmd_buffer.writeRef()));

    // Submit and present
    RHI_RETURN_ERROR_ON_FAIL(Renderer, m_queue->submit(cmd_buffer.get()));

    RHI_RETURN_ERROR_ON_FAIL(Renderer, m_surface->present());

    return default_value_v;
}
}
