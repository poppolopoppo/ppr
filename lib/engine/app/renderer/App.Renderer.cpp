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

    namespace {
        struct AttachmentInfo final {
            rhi::Format m_format{rhi::Format::Undefined};
            int2 m_extent{zero_v};
            u32 m_sample_count{0u};
        };

        [[nodiscard]] std::error_code inspectAttachment_(
            rhi::ITextureView *const view,
            AttachmentInfo *const out_info) noexcept {
            if (view == nullptr or out_info == nullptr) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            rhi::ITexture *const texture = view->getTexture();
            if (texture == nullptr) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            const rhi::TextureViewDesc &view_desc = view->getDesc();
            const rhi::TextureDesc &texture_desc = texture->getDesc();
            if (view_desc.subresourceRange.mip >= texture_desc.mipCount) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            const u32 mip = view_desc.subresourceRange.mip;
            const u32 width = std::max(1u, texture_desc.size.width >> mip);
            const u32 height = std::max(1u, texture_desc.size.height >> mip);
            const rhi::Format format =
                    view_desc.format == rhi::Format::Undefined
                        ? texture_desc.format
                        : view_desc.format;

            if (format == rhi::Format::Undefined or texture_desc.sampleCount == 0u) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            *out_info = AttachmentInfo{
                .m_format = format,
                .m_extent = int2{
                    safe_narrowing(width),
                    safe_narrowing(height),
                },
                .m_sample_count = texture_desc.sampleCount,
            };
            return default_value_v;
        }

        [[nodiscard]] std::error_code validateMatchingAttachment_(
            const AttachmentInfo &reference,
            const AttachmentInfo &candidate) noexcept {
            if (reference.m_extent.x != candidate.m_extent.x or
                reference.m_extent.y != candidate.m_extent.y or
                reference.m_sample_count != candidate.m_sample_count) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }
            return default_value_v;
        }

        void applyColorAttachmentOps_(
            rhi::RenderPassColorAttachment &attachment,
            const ColorAttachmentOps &ops) noexcept {
            attachment.loadOp = ops.m_load_op;
            attachment.storeOp = ops.m_store_op;
            attachment.clearValue[0] = ops.m_clear_color.x;
            attachment.clearValue[1] = ops.m_clear_color.y;
            attachment.clearValue[2] = ops.m_clear_color.z;
            attachment.clearValue[3] = ops.m_clear_color.w;
        }
    }

    std::error_code Renderer::initialize(IRhiService &rhi_service) {
        rhi::ComPtr<rhi::ICommandQueue> queue;
        rhi::IDevice &device = rhi_service.getDevice();
        PPR_RETURN_ERROR_ON_FAIL(Renderer, device.getQueue(rhi::QueueType::Graphics, queue.writeRef()));

        m_rhi_service = safe_ptr{&rhi_service};
        m_graphics_queue = std::move(queue);

        PPR_LOG(Renderer, info, "Renderer initialized");
        return default_value_v;
    }

    std::error_code Renderer::shutdown() {
        PPR_LOG(Renderer, info, "Renderer shut down", {
            {"surfaces", m_surfaces.size()},
            });

        std::error_code first_err{};
        PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, m_graphics_queue->waitOnHost());

        // flat_map iterates a pair-of-references proxy: take it by value.
        for (auto entry: m_surfaces) {
            SurfaceRecord &record = entry.second;
            if (record.m_configured and record.m_surface) {
                PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, record.m_surface->unconfigure());
                record.m_configured = false;
            }
            record.m_surface.setNull();
        }

        m_surfaces.clear();
        m_graphics_queue.setNull();
        m_rhi_service.reset();
        return first_err;
    }

    std::error_code Renderer::waitOnHost() {
        if (m_graphics_queue) {
            PPR_RETURN_ERROR_ON_FAIL(Renderer, m_graphics_queue->waitOnHost());
        }
        return default_value_v;
    }

    std::error_code Renderer::render(
        string_literal description,
        const rhi::RenderPassDesc &render_pass,
        const std::initializer_list<DrawSubmission> draws) {
        if (not m_graphics_queue or not m_rhi_service) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (render_pass.colorAttachmentCount != 0u and
            render_pass.colorAttachments == nullptr) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (render_pass.colorAttachmentCount == 0u and
            render_pass.depthStencilAttachment == nullptr) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        Array<rhi::Format, mem::ScratchPad> color_formats;
        color_formats.reserve(render_pass.colorAttachmentCount);

        std::optional<AttachmentInfo> reference_attachment;
        const auto validate_attachment = [&](const AttachmentInfo &attachment) -> std::error_code {
            if (not reference_attachment.has_value()) {
                reference_attachment = attachment;
                return default_value_v;
            }
            return validateMatchingAttachment_(*reference_attachment, attachment);
        };

        for (u32 index = 0u; index < render_pass.colorAttachmentCount; ++index) {
            const rhi::RenderPassColorAttachment &attachment = render_pass.colorAttachments[index];

            AttachmentInfo attachment_info{};
            PPR_RETURN_ERROR_ON_FAIL(Renderer, inspectAttachment_(attachment.view, &attachment_info));
            PPR_RETURN_ERROR_ON_FAIL(Renderer, validate_attachment(attachment_info));
            color_formats.push_back(attachment_info.m_format);

            if (attachment.resolveTarget != nullptr) {
                AttachmentInfo resolve_info{};
                PPR_RETURN_ERROR_ON_FAIL(Renderer, inspectAttachment_(attachment.resolveTarget, &resolve_info));

                if (attachment_info.m_sample_count <= 1u or
                    resolve_info.m_sample_count != 1u or
                    resolve_info.m_extent.x != attachment_info.m_extent.x or
                    resolve_info.m_extent.y != attachment_info.m_extent.y or
                    resolve_info.m_format != attachment_info.m_format) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
            }
        }

        std::optional<rhi::Format> depth_stencil_format;
        if (render_pass.depthStencilAttachment != nullptr) {
            AttachmentInfo depth_stencil_info{};
            PPR_RETURN_ERROR_ON_FAIL(Renderer, inspectAttachment_(
                render_pass.depthStencilAttachment->view, &depth_stencil_info));

            PPR_RETURN_ERROR_ON_FAIL(Renderer, validate_attachment(depth_stencil_info));
            depth_stencil_format = depth_stencil_info.m_format;
        }

        if (not reference_attachment.has_value()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        const RenderPipelineKey signature(RenderPipelineSignature{
            .m_color_formats = color_formats,
            .m_depth_stencil_format = depth_stencil_format,
            .m_sample_count = reference_attachment->m_sample_count,
        });

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, m_graphics_queue->createCommandEncoder(encoder.writeRef()));

        rhi::IRenderPassEncoder *const pass = encoder->beginRenderPass(render_pass);
        if (pass == nullptr) [[unlikely]] {
            return std::make_error_code(std::errc::io_error);
        }

        pass->pushDebugGroup(description.data(), rhi::MarkerColor{.r = 0.6f, .g = 0.3f, .b = 0.9f});

        // Pass scope: end() must precede finish().
        {
            PPR_DEFER {
                pass->popDebugGroup();
                pass->end();
            };

            const rhi::Viewport default_viewport = rhi::Viewport::fromSize(
                static_cast<float>(reference_attachment->m_extent.x),
                static_cast<float>(reference_attachment->m_extent.y));
            const rhi::ScissorRect default_scissor = rhi::ScissorRect::fromSize(
                safe_narrowing(reference_attachment->m_extent.x),
                safe_narrowing(reference_attachment->m_extent.y));

            for (const DrawSubmission &submission: draws) {
                pass->insertDebugMarker(submission.m_description.data(), rhi::MarkerColor{.r = 0.9f, .g = 0.3f, .b = 0.6f});

                const rhi::Viewport viewport = submission.m_viewport.value_or(default_viewport);
                const rhi::ScissorRect scissor = submission.m_scissor.value_or(default_scissor);

                rhi::RenderState state{};
                state.viewports[0] = viewport;
                state.viewportCount = 1u;
                state.scissorRects[0] = scissor;
                state.scissorRectCount = 1u;
                pass->setRenderState(state);

                PPR_RETURN_ERROR_ON_FAIL(Renderer, submission.m_encode_draws(DrawContext{
                    .m_device = m_rhi_service->getDevice(),
                    .m_pass = *pass,
                    .m_render_pipeline_key = signature,
                    .m_viewport = viewport,
                    .m_scissor = scissor,
                    .m_target_extent = reference_attachment->m_extent,
                    }));
            }
        }

        rhi::ComPtr<rhi::ICommandBuffer> command_buffer;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, encoder->finish(command_buffer.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(Renderer, m_graphics_queue->submit(command_buffer.get()));
        return default_value_v;
    }

    std::error_code Renderer::renderToTexture(
        rhi::ITexture &render_target,
        const std::initializer_list<DrawSubmission> draws,
        const ColorAttachmentOps &options) {
        // Hold the view: getDefaultView() returns an owning ComPtr and the
        // attachment only borrows it — binding the temporary dangles.
        rhi::ComPtr<rhi::ITextureView> target_view = render_target.getDefaultView();
        rhi::RenderPassColorAttachment color_attachment{};
        color_attachment.view = target_view.get();
        applyColorAttachmentOps_(color_attachment, options);

        const string_literal description{std::in_place, render_target.getDesc().label};
        return render(description, rhi::RenderPassDesc{
            .colorAttachments = &color_attachment,
            .colorAttachmentCount = 1u,
        }, draws);
    }

    std::error_code Renderer::renderAndPresent(
        const Window &window,
        const std::initializer_list<DrawSubmission> draws,
        const SurfaceRenderPass &surface_pass) {
        if (not m_graphics_queue or not m_rhi_service.isValid()) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (not window.m_handle) [[unlikely]] {
            return std::make_error_code(std::errc::no_such_device_or_address);
        }

        SurfaceRecord *record = nullptr;
        if (const auto it = m_surfaces.find(window.m_handle); it == m_surfaces.end()) [[unlikely]] {
            PPR_RETURN_ERROR_ON_FAIL(Renderer, createWindowSurface_(window, &record));
        } else {
            if (it->second.m_extent.x != window.m_framebuffer_size.x or
                it->second.m_extent.y != window.m_framebuffer_size.y) [[unlikely]] {
                PPR_RETURN_ERROR_ON_FAIL(Renderer, resizeWindowSurface_(it->second, window.m_framebuffer_size));
            }
            record = std::addressof(it->second);
        }

        if (not PPR_ENSURE(record->m_configured)) {
            return make_error_code(std::errc::resource_unavailable_try_again);
        }

        rhi::ComPtr<rhi::ITexture> image;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, record->m_surface->acquireNextImage(image.writeRef()));

        Array<rhi::RenderPassColorAttachment, mem::ScratchPad> color_attachments;
        color_attachments.reserve(1u + surface_pass.m_additional_colors.size());

        rhi::RenderPassColorAttachment surface_color{};
        surface_color.view = image->getDefaultView();
        applyColorAttachmentOps_(surface_color, surface_pass.m_surface_color);
        color_attachments.push_back(surface_color);

        for (const rhi::RenderPassColorAttachment &attachment: surface_pass.m_additional_colors) {
            color_attachments.push_back(attachment);
        }

        const rhi::RenderPassDesc render_pass{
            .colorAttachments = color_attachments.data(),
            .colorAttachmentCount = safe_narrowing(color_attachments.size()),
            .depthStencilAttachment = surface_pass.m_depth_stencil
                                          ? std::addressof(*surface_pass.m_depth_stencil)
                                          : nullptr,
        };

        std::error_code first_err{};
        const string_literal description{std::in_place, window.m_title.data()};
        PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, render(description, render_pass, draws));
        PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, record->m_surface->present());
        return first_err;
    }

    std::error_code Renderer::destroyWindowSurface(const Window &window) {
        if (not window.m_handle) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return destroyWindowSurface_(window.m_handle);
    }

    std::error_code Renderer::createWindowSurface_(const Window &window, SurfaceRecord **out_record) {
        if (out_record == nullptr or
            not m_rhi_service.isValid() or
            window.m_native == nullptr or
            window.m_handle == nullptr)
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        rhi::ComPtr<rhi::ISurface> surface;
        rhi::IDevice &device = m_rhi_service->getDevice();
        PPR_RETURN_ERROR_ON_FAIL(Renderer, device.createSurface(
            rhi::WindowHandle::fromHwnd(window.m_native),
            surface.writeRef()));

        SurfaceRecord record{};
        record.m_surface = std::move(surface);
        PPR_RETURN_ERROR_ON_FAIL(Renderer, resizeWindowSurface_(record, window.m_framebuffer_size));

        const auto [it, inserted] = m_surfaces.insert_or_assign(window.m_handle, std::move(record));
        PPR_ASSERT(inserted);
        *out_record = std::addressof(it->second);

        PPR_LOG(Renderer, info, "window surface created", {
            {"description", window.m_title},
            {"width", it->second.m_extent.x},
            {"height", it->second.m_extent.y},
            });
        return default_value_v;
    }

    std::error_code Renderer::resizeWindowSurface_(SurfaceRecord &record, const int2 &new_extent) {
        PPR_ASSERT(record.m_surface);

        if (new_extent.x <= 0 or new_extent.y <= 0) {
            if (record.m_configured) {
                PPR_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->unconfigure());
                record.m_configured = false;
            }

            record.m_extent = new_extent;
            return default_value_v;
        }

        if (record.m_configured) {
            PPR_RETURN_ERROR_ON_FAIL(Renderer, waitOnHost());
        }

        rhi::SurfaceConfig surface_config{};
        surface_config.width = safe_narrowing(new_extent.x);
        surface_config.height = safe_narrowing(new_extent.y);
        surface_config.format = m_preferred_surface_format;
        surface_config.desiredImageCount = m_desired_image_count;
        surface_config.vsync = m_enable_vsync;

        PPR_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->configure(surface_config));

        record.m_extent = new_extent;
        record.m_configured = true;

        PPR_LOG(Renderer, info, "window surface resized", {
            {"format", rhi::getFormatInfo(record.m_surface->getInfo().preferredFormat).name},
            {"width", record.m_extent.x},
            {"height", record.m_extent.y},
            });
        return default_value_v;
    }

    std::error_code Renderer::destroyWindowSurface_(const WindowHandle window_handle) {
        const auto it = m_surfaces.find(window_handle);
        if (it == m_surfaces.end()) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        SurfaceRecord record = std::move(it->second);
        m_surfaces.erase(it);

        std::error_code first_error{};
        if (record.m_configured and record.m_surface) {
            PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_error, record.m_surface->unconfigure());
            record.m_configured = false;
        }

        record.m_surface.setNull();
        return first_error;
    }
}
