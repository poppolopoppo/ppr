module;
#include "pP/Macros.h"
module engine.app;

import :renderer;
import :renderer.types;
import :service.window;
import :window.handle;
import std;
import engine.core;
import engine.math;
import engine.rhi;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Renderer, info, none)

    rhi::Format Renderer::getWindowSurfaceFormat(const WindowHandle handle) const noexcept {
        const auto it = m_surfaces.find(handle);
        if (it == m_surfaces.end())
            return rhi::Format::Undefined;
        return it->second.m_format;
    }

    std::error_code Renderer::initialize(IRhiService &rhi_service) {
        m_rhi_service = safe_ptr{&rhi_service};

        RHI_RETURN_ERROR_ON_FAIL(Renderer, rhi_service.getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef()));

        PPR_LOG(Renderer, info, "Renderer initialized", {
            {"has_queue", m_queue != nullptr},
        });
        return default_value_v;
    }

    std::error_code Renderer::createWindowSurface(IWindowService &window_service, const Window &window) {
        PPR_ASSERT(m_queue);
        PPR_ASSERT(m_rhi_service.isValid());

        void *const native = window_service.getWindowNativeHandle(window);
        if (native == nullptr) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        const WindowHandle handle = window.m_handle;
        if (handle == WindowHandle{}) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        if (const auto it = m_surfaces.find(handle); it != m_surfaces.end()) {
            // Re-registration: reuse the surface, refresh its configuration.
            PPR_RETURN_ERROR_ON_FAIL(Renderer, configureSurface_(it->second, window.m_framebuffer_size));
            return default_value_v;
        }

        rhi::ComPtr<rhi::ISurface> surface;
        RHI_RETURN_ERROR_ON_FAIL(
            Renderer,
            m_rhi_service->getDevice().createSurface(toRhiWindowHandle_(native), surface.writeRef()));

        SurfaceRecord record{};
        record.m_surface = std::move(surface);
        PPR_RETURN_ERROR_ON_FAIL(Renderer, configureSurface_(record, window.m_framebuffer_size));

        const int2 size = record.m_size;
        m_surfaces.insert_or_assign(handle, std::move(record));

        PPR_LOG(Renderer, info, "window surface created", {
            {"width", size.x},
            {"height", size.y},
        });
        return default_value_v;
    }

    std::error_code Renderer::destroyWindowSurface(const WindowHandle handle) {
        const auto it = m_surfaces.find(handle);
        if (it == m_surfaces.end()) {
            // Idempotent: unknown or already destroyed.
            return default_value_v;
        }

        SurfaceRecord record = std::move(it->second);
        m_surfaces.erase(it);

        if (record.m_configured and record.m_surface) {
            RHI_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->unconfigure());
        }
        record.m_surface.setNull();
        return default_value_v;
    }

    std::error_code Renderer::resizeWindowSurface(const WindowHandle handle, const int2 size) {
        const auto it = m_surfaces.find(handle);
        if (it == m_surfaces.end()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        if (size.x <= 0 or size.y <= 0) {
            // Minimized: tear down the swapchain, keep the record.
            return configureSurface_(it->second, size);
        }

        PPR_RETURN_ERROR_ON_FAIL(Renderer, waitForIdle());
        PPR_RETURN_ERROR_ON_FAIL(Renderer, configureSurface_(it->second, size));

        PPR_LOG(Renderer, info, "window surface resized", {
            {"width", size.x},
            {"height", size.y},
        });
        return default_value_v;
    }

    std::error_code Renderer::renderAndPresent(
        const WindowHandle handle,
        const std::span<const DrawSubmission> draws,
        const ColorPassOptions &options) {
        PPR_ASSERT(m_queue);

        const auto it = m_surfaces.find(handle);
        if (it == m_surfaces.end()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        SurfaceRecord &record = it->second;
        if (not record.m_configured) {
            // Minimized/unconfigured: success no-op.
            return default_value_v;
        }

        rhi::ComPtr<rhi::ITexture> image;
        RHI_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->acquireNextImage(image.writeRef()));

        const rhi::TextureDesc &image_desc = image->getDesc();
        const int2 extent{static_cast<i32>(image_desc.size.width), static_cast<i32>(image_desc.size.height)};
        PPR_RETURN_ERROR_ON_FAIL(
            Renderer,
            submitToTarget_(*image, ColorTargetInfo{image_desc.format, extent, image_desc.sampleCount}, draws, options));

        RHI_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->present());
        return default_value_v;
    }

    std::error_code Renderer::submitToTexture(
        rhi::ITexture &target,
        const std::span<const DrawSubmission> draws,
        const ColorPassOptions &options) {
        PPR_ASSERT(m_queue);

        const rhi::TextureDesc &desc = target.getDesc();
        const int2 extent{static_cast<i32>(desc.size.width), static_cast<i32>(desc.size.height)};
        return submitToTarget_(target, ColorTargetInfo{desc.format, extent, desc.sampleCount}, draws, options);
    }

    std::error_code Renderer::waitForIdle() {
        if (m_queue) {
            RHI_RETURN_ERROR_ON_FAIL(Renderer, m_queue->waitOnHost());
        }
        return default_value_v;
    }

    std::error_code Renderer::shutdown() {
        PPR_LOG(Renderer, info, "Renderer shut down", {
            {"surfaces", m_surfaces.size()},
        });

        // Retain-first-error: every teardown step runs, the first failure wins.
        std::error_code first{};
        const auto retain = [&](const std::error_code &ec) noexcept {
            if (hasFailed(ec) and not hasFailed(first)) {
                first = ec;
            }
        };

        retain(waitForIdle());

        // flat_map iterates a pair-of-references proxy: take it by value.
        for (auto entry: m_surfaces) {
            SurfaceRecord &record = entry.second;
            if (record.m_configured and record.m_surface) {
                retain(rhi::make_error_code(record.m_surface->unconfigure()));
                record.m_configured = false;
            }
            record.m_surface.setNull();
        }
        m_surfaces.clear();

        m_queue.setNull();
        m_rhi_service.reset();
        return first;
    }

    std::error_code Renderer::configureSurface_(SurfaceRecord &record, const int2 size) {
        PPR_ASSERT(record.m_surface);

        if (size.x <= 0 or size.y <= 0) {
            if (record.m_configured) {
                RHI_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->unconfigure());
                record.m_configured = false;
            }
            record.m_size = size;
            return default_value_v;
        }

        rhi::SurfaceConfig surface_config{};
        surface_config.width = static_cast<u32>(size.x);
        surface_config.height = static_cast<u32>(size.y);
        surface_config.desiredImageCount = 3;
        surface_config.vsync = true;

        RHI_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->configure(surface_config));

        record.m_size = size;
        record.m_format = record.m_surface->getInfo().preferredFormat;
        record.m_configured = true;
        return default_value_v;
    }

    std::error_code Renderer::submitToTarget_(
        rhi::ITexture &target,
        const ColorTargetInfo &target_info,
        const std::span<const DrawSubmission> draws,
        const ColorPassOptions &options) {
        PPR_ASSERT(m_queue);

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        RHI_RETURN_ERROR_ON_FAIL(Renderer, m_queue->createCommandEncoder(encoder.writeRef()));

        rhi::RenderPassColorAttachment color_attachment{};
        color_attachment.view = target.getDefaultView();
        color_attachment.loadOp = options.m_load_op;
        color_attachment.storeOp = options.m_store_op;
        color_attachment.clearValue[0] = options.m_clear_color[0];
        color_attachment.clearValue[1] = options.m_clear_color[1];
        color_attachment.clearValue[2] = options.m_clear_color[2];
        color_attachment.clearValue[3] = options.m_clear_color[3];

        rhi::RenderPassDesc render_pass_desc{};
        render_pass_desc.colorAttachments = &color_attachment;
        render_pass_desc.colorAttachmentCount = 1;

        rhi::IRenderPassEncoder *const pass = encoder->beginRenderPass(render_pass_desc);
        PPR_ASSERT(pass != nullptr);
        {
            PPR_DEFER {
                pass->end();
            };

            PPR_RETURN_ERROR_ON_FAIL(Renderer, encodeDraws_(*pass, target_info, draws));
        }

        rhi::ComPtr<rhi::ICommandBuffer> cmd_buffer;
        RHI_RETURN_ERROR_ON_FAIL(Renderer, encoder->finish(cmd_buffer.writeRef()));

        // Submit without waiting; present() or an explicit waitForIdle() synchronizes.
        RHI_RETURN_ERROR_ON_FAIL(Renderer, m_queue->submit(cmd_buffer.get()));
        return default_value_v;
    }

    std::error_code Renderer::encodeDraws_(
        rhi::IRenderPassEncoder &pass,
        const ColorTargetInfo &target_info,
        const std::span<const DrawSubmission> draws) {
        for (const DrawSubmission &submission: draws) {
            pass.setRenderState({
                .viewports = {submission.m_view.m_viewport},
                .viewportCount = 1,
                .scissorRects = {submission.m_view.m_scissor},
                .scissorRectCount = 1,
            });

            const DrawContext context{submission.m_view, target_info};
            PPR_RETURN_ERROR_ON_FAIL(Renderer, submission.m_encode_draws(pass, context));
        }
        return default_value_v;
    }

    rhi::WindowHandle Renderer::toRhiWindowHandle_(void *const native) noexcept {
#if defined(_WIN32)
        // The GLFW backend exposes the Win32 HWND. Other backends need their
        // own arm here (NSWindow/Xlib); an Undefined handle fails loudly in
        // createSurface, surfacing as error_code from createWindowSurface.
        return rhi::WindowHandle::fromHwnd(native);
#else
        (void) native;
        return rhi::WindowHandle{};
#endif
    }
}
