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
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Renderer, info, none)

    // ------------------------------------------------------------------
    // attachment validation and setup
    // ------------------------------------------------------------------

    namespace {
        struct AttachmentInfo final {
            rhi::Format m_format{rhi::Format::Undefined};
            int2 m_extent{zero_v};
            u32 m_sample_count{0u};
        };

        // "All remaining" layers/mips sentinel (mirrors the slang-rhi
        // kAllLayers/kAllMips values, which engine.rhi does not re-export;
        // app code stays behind the engine.rhi seam).
        inline constexpr u32 kAllRemaining = 0xFFFFFFFFu;

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

            // Attachments are single slices of plain (optionally multisampled)
            // 2D textures: array, 3D, and cube views fail closed, as do views
            // spanning multiple mips or layers — including kEntireTexture over
            // such textures. Engine attachment sources are single-mip 2D
            // targets, so their default views keep passing.
            if ((texture_desc.type != rhi::TextureType::Texture2D and
                 texture_desc.type != rhi::TextureType::Texture2DMS) or
                texture_desc.arrayLength != 1u or
                texture_desc.size.depth != 1u) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            const rhi::SubresourceRange &range = view_desc.subresourceRange;
            if (range.mip >= texture_desc.mipCount or range.layer >= texture_desc.arrayLength) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }
            const u32 mip_span =
                range.mipCount == kAllRemaining ? texture_desc.mipCount - range.mip : range.mipCount;
            const u32 layer_span =
                range.layerCount == kAllRemaining ? texture_desc.arrayLength - range.layer : range.layerCount;
            if (mip_span != 1u or layer_span != 1u) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            // No max(1u, ...) masking: a shifted-out extent means a malformed
            // descriptor and fails closed instead of silently attaching 1x1.
            const u32 width = texture_desc.size.width >> range.mip;
            const u32 height = texture_desc.size.height >> range.mip;
            if (width == 0u or height == 0u) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

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
            if (reference.m_extent.x != candidate.m_extent.x or reference.m_extent.y != candidate.m_extent.y or
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

        // Validates one render pass descriptor into a pipeline signature plus
        // the reference extent that derives the default viewport/scissor. Pure
        // and queue-free, so a caller validates before touching an encoder and
        // keeps an invalid pass out of command recording entirely.
        [[nodiscard]] std::error_code buildRenderSignature_(
            const rhi::RenderPassDesc &render_pass,
            RenderPipelineSignature &out_signature,
            int2 &out_target_extent) {
            if (render_pass.colorAttachmentCount != 0u and render_pass.colorAttachments == nullptr) {
                return std::make_error_code(std::errc::invalid_argument);
            }
            if (render_pass.colorAttachmentCount == 0u and render_pass.depthStencilAttachment == nullptr) {
                return std::make_error_code(std::errc::invalid_argument);
            }
            // Fail closed: reject over-capacity instead of truncating to kMaxColorFormats.
            if (render_pass.colorAttachmentCount > RenderPipelineSignature::kMaxColorFormats) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            RenderPipelineSignature signature{};

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
                if (signature.m_color_format_count >= RenderPipelineSignature::kMaxColorFormats) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                signature.m_color_formats[signature.m_color_format_count++] = attachment_info.m_format;

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

            if (render_pass.depthStencilAttachment != nullptr) {
                AttachmentInfo depth_stencil_info{};
                PPR_RETURN_ERROR_ON_FAIL(Renderer, inspectAttachment_(
                    render_pass.depthStencilAttachment->view, &depth_stencil_info));

                PPR_RETURN_ERROR_ON_FAIL(Renderer, validate_attachment(depth_stencil_info));
                signature.m_depth_stencil_format = depth_stencil_info.m_format;
            }

            if (not reference_attachment.has_value()) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            // The only u32 -> SampleCount seam: a non-power-of-two backbuffer
            // or attachment count fails closed here, before the signature is
            // published to any cache or pipeline.
            const Expected<SampleCount> sample_count =
                    makeSampleCount(reference_attachment->m_sample_count);
            if (not sample_count) [[unlikely]] {
                return sample_count.error();
            }
            signature.m_sample_count = *sample_count;

            out_signature = signature;
            out_target_extent = reference_attachment->m_extent;
            return default_value_v;
        }

        // One surface pass fully built ahead of encoding: the attachment
        // storage its descriptor points into plus the validated signature and
        // the reference extent. Phase 1 of renderAndPresent fills these; phase
        // 2 encodes them, so no pass can observe a half-recorded frame. The
        // vector is reserved before its first push_back, so neither the
        // elements nor the inner attachment buffers ever move after their
        // addresses have been published into a descriptor.
        struct BuiltSurfacePass final {
            Array<rhi::RenderPassColorAttachment, mem::ScratchPad> m_color_attachments{};
            std::optional<rhi::RenderPassDepthStencilAttachment> m_depth_stencil{};
            rhi::RenderPassDesc m_desc{};
            std::span<const DrawSubmission> m_draws{};
            RenderPipelineSignature m_signature{};
            int2 m_extent{zero_v};
        };

        // Encodes exactly one render pass into `encoder`: begin, description
        // debug group, the draw submissions, then pop/end on every exit path.
        // The pass is closed even when a submission fails, so the caller may
        // hand the same encoder to the next pass or abandon it without
        // finish(). Failures are already logged at their innermost site.
        [[nodiscard]] std::error_code encodeRenderPass_(
            rhi::ICommandEncoder &encoder,
            string_literal description,
            const rhi::RenderPassDesc &render_pass,
            const RenderPipelineSignature &pipeline_signature,
            const int2 &target_extent,
            const std::span<const DrawSubmission> draws,
            rhi::IDevice &device) {
            rhi::IRenderPassEncoder *const pass = encoder.beginRenderPass(render_pass);
            if (pass == nullptr) [[unlikely]] {
                PPR_LOG(Renderer, error, "beginRenderPass returned null", {
                    {"description", description.data()},
                    });
                return std::make_error_code(std::errc::io_error);
            }

            pass->pushDebugGroup(description.data(), rhi::MarkerColor{.r = 0.6f, .g = 0.3f, .b = 0.9f});

            // Pass scope: end() must precede finish().
            {
                PPR_DEFER {
                    pass->popDebugGroup();
                    pass->end();
                };

                const RenderPipelineKey signature(pipeline_signature);

                const rhi::Viewport default_viewport = rhi::Viewport::fromSize(
                    static_cast<float>(target_extent.x),
                    static_cast<float>(target_extent.y));
                const rhi::ScissorRect default_scissor = rhi::ScissorRect::fromSize(
                    safe_narrowing(target_extent.x),
                    safe_narrowing(target_extent.y));

                rhi::MarkerColor submission_color{.r = 0.9f, .g = 0.3f, .b = 0.6f};
                for (const DrawSubmission &submission: draws) {
                    pass->pushDebugGroup(submission.m_description.data(), submission_color);
                    PPR_DEFER {
                        pass->popDebugGroup();

                        submission_color.r = fract(submission_color.r + phi_v<float>);
                        submission_color.g = fract(submission_color.g + phi_v<float>);
                        submission_color.b = fract(submission_color.b + phi_v<float>);
                    };

                    const rhi::Viewport viewport = submission.m_viewport.value_or(default_viewport);
                    const rhi::ScissorRect scissor = submission.m_scissor.value_or(default_scissor);

                    rhi::RenderState state{};
                    state.viewports[0] = viewport;
                    state.viewportCount = 1u;
                    state.scissorRects[0] = scissor;
                    state.scissorRectCount = 1u;
                    pass->setRenderState(state);

                    PPR_RETURN_ERROR_ON_FAIL(Renderer, submission.m_encode_draws(DrawContext{
                        .m_device = device,
                        .m_pass = *pass,
                        .m_render_pipeline_key = signature,
                        .m_viewport = viewport,
                        .m_scissor = scissor,
                        .m_target_extent = target_extent,
                        }));
                }
            }
            return default_value_v;
        }
    }

    // ------------------------------------------------------------------
    // renderer lifecycle and submission
    // ------------------------------------------------------------------

    std::error_code Renderer::initialize(IRhiService &rhi_service) {
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        rhi::ComPtr<rhi::ICommandQueue> queue;
        rhi::IDevice &device = rhi_service.getDevice();
        PPR_RETURN_ERROR_ON_FAIL(Renderer, device.getQueue(rhi::QueueType::Graphics, queue.writeRef()));

        m_rhi_service = safe_ptr{&rhi_service};
        m_graphics_queue = std::move(queue);
        m_owner = std::this_thread::get_id();

        PPR_LOG(Renderer, info, "Renderer initialized");
        return default_value_v;
    }

    std::error_code Renderer::shutdown() {
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        PPR_LOG(Renderer, info, "Renderer shut down", {
            {"surfaces", m_surfaces.size()},
            });

        std::error_code first_err{};
        PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, waitOnHost());

        // The FlatMap iterator yields a proxy/prvalue pair, so take entries by
        // value: binding to `auto&` fails. unconfigure() acts on the shared
        // object via ComPtr and `m_surfaces.clear()` performs the real release.
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
        m_owner = std::thread::id{};
        return first_err;
    }

    bool Renderer::onRenderThread_() const noexcept {
        return std::this_thread::get_id() == m_owner;
    }

    std::error_code Renderer::waitOnHost() {
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        if (m_graphics_queue) {
            PPR_RETURN_ERROR_ON_FAIL(Renderer, m_graphics_queue->waitOnHost());
        }
        return default_value_v;
    }

    std::error_code Renderer::render(
        string_literal description,
        const rhi::RenderPassDesc &render_pass,
        const std::initializer_list<DrawSubmission> draws) {
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        return renderDraws_(description, render_pass, std::span{draws.begin(), draws.size()});
    }

    std::error_code Renderer::renderDraws_(string_literal description, const rhi::RenderPassDesc &render_pass,
                                           const std::span<const DrawSubmission> draws) {
        if (not m_graphics_queue or not m_rhi_service.isValid()) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }

        const auto scope = mem::ScratchPad::open();

        RenderPipelineSignature pipeline_signature{};
        int2 target_extent{zero_v};
        if (const std::error_code signature_err =
                buildRenderSignature_(render_pass, pipeline_signature, target_extent)) [[unlikely]] {
            return signature_err;
        }

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, m_graphics_queue->createCommandEncoder(encoder.writeRef()));

        if (const std::error_code encode_err = encodeRenderPass_(*encoder, description, render_pass,
            pipeline_signature, target_extent, draws,
            m_rhi_service->getDevice())) [[unlikely]] {
            return encode_err;
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
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        // Hold the view: getDefaultView() returns an owning ComPtr and the
        // attachment only borrows it — binding the temporary dangles.
        const rhi::ComPtr<rhi::ITextureView> target_view = render_target.getDefaultView();
        rhi::RenderPassColorAttachment color_attachment{};
        color_attachment.view = target_view.get();
        applyColorAttachmentOps_(color_attachment, options);

        const string_literal description{std::in_place, render_target.getDesc().label};
        return render(description, rhi::RenderPassDesc{
            .colorAttachments = &color_attachment,
            .colorAttachmentCount = 1u,
        }, draws);
    }

    // ------------------------------------------------------------------
    // window surface lifecycle and presentation
    // ------------------------------------------------------------------

    // Surface-hoisted pipeline-signature resolution, one call per pass in
    // phase 1. A signature is a function of the surface shape ONLY — (color
    // formats, sample count, depth format) — so it changes on resize, never per
    // frame: resizeWindowSurface_ recomputes the cached entries and a
    // steady-state frame reuses them here. Attachments are still validated on
    // every frame — the fail-closed kMaxColorFormats check stays on the
    // per-frame path, because the cache stores the signature VALUE and never a
    // "validated" verdict.
    //
    // The depth policy picks the cache slot (none / renderer_owned): the two
    // pass shapes keep separate entries, and external depth plus any pass
    // carrying extra color attachments is caller-shaped and never cached. A
    // stored entry is reused only while it still matches this frame's validated
    // pass and the surface's current extent; any disagreement records a shape
    // change that never went through a resize, so the entry is recomputed
    // (fail-safe) instead of a stale value being published.
    std::error_code Renderer::surfaceSignature_(
        SurfaceRecord &record,
        const SurfaceRenderPass &surface_pass,
        const rhi::RenderPassDesc &render_pass,
        RenderPipelineSignature &out_signature,
        int2 &out_target_extent) {
        RenderPipelineSignature validated{};
        int2 validated_extent{zero_v};
        PPR_RETURN_ERROR_ON_FAIL(Renderer, buildRenderSignature_(render_pass, validated, validated_extent));

        SurfaceSignatureCache *cache = nullptr;
        if (surface_pass.m_depth_policy != ESurfaceDepthPolicy::external and
            surface_pass.m_additional_colors.empty()) {
            cache = std::addressof(
                record.m_signatures[static_cast<std::size_t>(surface_pass.m_depth_policy)]);
        }

        const bool reusable =
                cache != nullptr and cache->m_valid and
                cache->m_depth_policy == surface_pass.m_depth_policy and
                cache->m_extent.x == record.m_extent.x and cache->m_extent.y == record.m_extent.y and
                cache->m_extent.x == validated_extent.x and cache->m_extent.y == validated_extent.y and
                cache->m_signature == validated;
        if (reusable) {
            out_signature = cache->m_signature;
            out_target_extent = cache->m_extent;
            return default_value_v;
        }

        out_signature = validated;
        out_target_extent = validated_extent;
        if (cache != nullptr) {
            cache->m_depth_policy = surface_pass.m_depth_policy;
            cache->m_signature = validated;
            cache->m_extent = validated_extent;
            cache->m_valid = true;
        }
        return default_value_v;
    }

    std::error_code Renderer::renderAndPresent(
        const Window &window,
        const std::initializer_list<SurfaceRenderPass> passes) {
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
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
            if (it->second.m_extent.x != window.m_framebuffer_size.x or it->second.m_extent.y != window.m_framebuffer_size.y) [[unlikely]] {
                PPR_RETURN_ERROR_ON_FAIL(Renderer, resizeWindowSurface_(it->second, window.m_framebuffer_size));
            }
            record = std::addressof(it->second);
        }

        // m_configured is the in-flight-work token: no claim means nothing was submitted, so restore may skip the fence.
        if (not PPR_ENSURE(record->m_configured)) {
            return make_error_code(std::errc::resource_unavailable_try_again);
        }

        if (passes.empty()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        for (const SurfaceRenderPass &surface_pass: passes) {
            const bool invalid_depth_policy =
                    (surface_pass.m_depth_policy == ESurfaceDepthPolicy::none and surface_pass.m_depth_stencil) or
                    (surface_pass.m_depth_policy == ESurfaceDepthPolicy::renderer_owned and (
                         surface_pass.m_depth_stencil or not record->m_depth_texture or not record->m_depth_view)) or
                    (surface_pass.m_depth_policy == ESurfaceDepthPolicy::external and not surface_pass.m_depth_stencil);
            if (invalid_depth_policy) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }
        }

        rhi::ComPtr<rhi::ITexture> image;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, record->m_surface->acquireNextImage(image.writeRef()));
        const rhi::ComPtr<rhi::ITextureView> image_view = image->getDefaultView();

        // Fail early when the acquired backbuffer no longer matches the
        // resize-matched depth view instead of tripping per-frame validation.
        // Only passes selecting renderer-owned depth need the pair to match.
        bool wants_owned_depth = false;
        for (const SurfaceRenderPass &surface_pass: passes) {
            if (surface_pass.m_depth_policy == ESurfaceDepthPolicy::renderer_owned) {
                wants_owned_depth = true;
                break;
            }
        }

        if (wants_owned_depth) {
            AttachmentInfo acquired_info{};
            PPR_RETURN_ERROR_ON_FAIL(Renderer, inspectAttachment_(image_view.get(), &acquired_info));
            if (record->m_depth_view) {
                AttachmentInfo depth_info{};
                PPR_RETURN_ERROR_ON_FAIL(Renderer, inspectAttachment_(record->m_depth_view.get(), &depth_info));
                if (acquired_info.m_extent.x != depth_info.m_extent.x or
                    acquired_info.m_extent.y != depth_info.m_extent.y) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
            }
        }

        std::error_code first_err{};
        const string_literal description{std::in_place, window.m_title.data()};

        // One arena scope for both phases: every descriptor built below points
        // into attachment storage owned by built_passes, and that storage must
        // outlive the encoder that records from it.
        const auto frame_scope = mem::ScratchPad::open();

        // Phase 1 — build. Assemble every pass's color attachments, depth
        // attachment, and descriptor, then resolve its pipeline signature and
        // reference extent, all before anything is recorded. The first failure
        // stops the build, so phase 2 never runs against a partial frame.
        Array<BuiltSurfacePass, mem::ScratchPad> built_passes;
        built_passes.reserve(passes.size());

        for (const SurfaceRenderPass &surface_pass: passes) {
            built_passes.push_back(BuiltSurfacePass{});
            BuiltSurfacePass &built = built_passes.back();

            built.m_color_attachments.reserve(1u + surface_pass.m_additional_colors.size());

            rhi::RenderPassColorAttachment surface_color{};
            surface_color.view = image_view.get();
            applyColorAttachmentOps_(surface_color, surface_pass.m_surface_color);
            built.m_color_attachments.push_back(surface_color);

            for (const rhi::RenderPassColorAttachment &attachment: surface_pass.m_additional_colors) {
                built.m_color_attachments.push_back(attachment);
            }

            switch (surface_pass.m_depth_policy) {
                case ESurfaceDepthPolicy::none:
                    break;
                case ESurfaceDepthPolicy::renderer_owned:
                    built.m_depth_stencil = rhi::RenderPassDepthStencilAttachment{};
                    built.m_depth_stencil->view = record->m_depth_view.get();
                    break;
                case ESurfaceDepthPolicy::external:
                    built.m_depth_stencil = *surface_pass.m_depth_stencil;
                    break;
            }

            built.m_desc = rhi::RenderPassDesc{
                .colorAttachments = built.m_color_attachments.data(),
                .colorAttachmentCount = safe_narrowing(built.m_color_attachments.size()),
                .depthStencilAttachment = built.m_depth_stencil ? std::addressof(*built.m_depth_stencil) : nullptr,
            };
            built.m_draws = std::span<const DrawSubmission>{
                surface_pass.m_draws.begin(), surface_pass.m_draws.size()
            };

            first_err = surfaceSignature_(*record, surface_pass, built.m_desc,
                built.m_signature, built.m_extent);
            if (first_err) {
                break;
            }
        }

        // Phase 2 — encode. One encoder serves every pass of the frame: each
        // pass opens and closes its own render pass on it, then a single
        // finish + submit follows the loop. Every failure is retained rather
        // than returned so the already-acquired image still presents. A failed
        // pass skips every remaining pass and abandons the encoder without
        // finish/submit: the encoded-but-unsubmitted work is discarded when
        // the encoder is destroyed, while the present below still runs.
        if (not first_err) {
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err,
                m_graphics_queue->createCommandEncoder(encoder.writeRef()));

            for (const BuiltSurfacePass &built: built_passes) {
                if (first_err) {
                    break;
                }
                first_err = encodeRenderPass_(*encoder, description, built.m_desc, built.m_signature,
                    built.m_extent, built.m_draws,
                    m_rhi_service->getDevice());
            }

            if (not first_err) {
                rhi::ComPtr<rhi::ICommandBuffer> command_buffer;
                PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, encoder->finish(command_buffer.writeRef()));
                if (not first_err) {
                    PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, m_graphics_queue->submit(command_buffer.get()));
                }
            }
        }

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
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (out_record == nullptr or
            not m_rhi_service.isValid() or
            window.m_native == nullptr or
            window.m_handle == nullptr) [[unlikely]] {
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

        const auto [it, inserted] = m_surfaces.try_emplace(window.m_handle, std::move(record));
        if (not inserted) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
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
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        // The cached entries describe the old surface shape, so every slot is
        // invalidated before the surface is touched: the minimize and error
        // paths below leave them invalid (the next frame rebuilds from live
        // attachments), while the success path reseeds them from the new
        // configure and depth texture.
        for (SurfaceSignatureCache &cache: record.m_signatures) {
            cache.m_valid = false;
        }

        // Fence the GPU before touching the surface so the
        // minimize/unconfigure path below cannot race in-flight work.
        if (record.m_configured) {
            PPR_RETURN_ERROR_ON_FAIL(Renderer, waitOnHost());
        }

        if (new_extent.x <= 0 or new_extent.y <= 0) {
            if (record.m_configured) {
                const rhi::Result unconfigure_result = record.m_surface->unconfigure();
                record.m_configured = false;
                PPR_RETURN_ERROR_ON_FAIL(Renderer, unconfigure_result);
            }

            record.m_depth_view.setNull();
            record.m_depth_texture.setNull();
            record.m_extent = new_extent;
            return default_value_v;
        }

        rhi::SurfaceConfig surface_config{};
        surface_config.width = safe_narrowing(new_extent.x);
        surface_config.height = safe_narrowing(new_extent.y);
        surface_config.format = m_preferred_surface_format;
        surface_config.desiredImageCount = m_desired_image_count;
        surface_config.vsync = m_enable_vsync;

        rhi::ComPtr<rhi::ITexture> depth_texture;
        rhi::ComPtr<rhi::ITextureView> depth_view;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, createSurfaceDepth_(new_extent, depth_texture, depth_view));

        // Drop the configured claim before reconfiguration so a failed
        // configure cannot leave a stale configured record behind.
        record.m_configured = false;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, record.m_surface->configure(surface_config));

        record.m_depth_texture = std::move(depth_texture);
        record.m_depth_view = std::move(depth_view);
        record.m_extent = new_extent;
        record.m_configured = true;

        // The surface shape is fixed by this configure and this depth texture,
        // so the cacheable signature entries are reseeded here — the only place
        // they can change — instead of being rebuilt on every frame.
        recomputeSurfaceSignatures_(record);

        PPR_LOG(Renderer, info, "window surface resized", {
            {"format", rhi::getFormatInfo(record.m_surface->getInfo().preferredFormat).name},
            {"width", record.m_extent.x},
            {"height", record.m_extent.y},
            });
        return default_value_v;
    }

    // Reseeds the cacheable per-pass signature entries from the inputs that
    // actually define them: the configured swapchain format, the backbuffer
    // sample count, and the depth texture's format. SurfaceConfig carries no
    // sample-count field — swapchain images cannot be multisampled — so the
    // backbuffer is single-sampled; a value that ever disagrees with the
    // per-frame validation is caught by the reuse comparison and recomputed
    // there instead of being published stale.
    void Renderer::recomputeSurfaceSignatures_(SurfaceRecord &record) {
        const rhi::Format surface_format =
                m_preferred_surface_format == rhi::Format::Undefined
                    ? record.m_surface->getInfo().preferredFormat
                    : m_preferred_surface_format;
        if (surface_format == rhi::Format::Undefined) [[unlikely]] {
            return;
        }

        const auto seed = [&](const ESurfaceDepthPolicy policy, const std::optional<rhi::Format> depth_format) {
            RenderPipelineSignature signature{};
            signature.m_color_formats[0] = surface_format;
            signature.m_color_format_count = 1u;
            signature.m_sample_count = SampleCount::x1;
            signature.m_depth_stencil_format = depth_format;

            SurfaceSignatureCache &cache = record.m_signatures[static_cast<std::size_t>(policy)];
            cache.m_depth_policy = policy;
            cache.m_signature = signature;
            cache.m_extent = record.m_extent;
            cache.m_valid = true;
        };

        seed(ESurfaceDepthPolicy::none, std::nullopt);
        if (record.m_depth_texture) {
            seed(ESurfaceDepthPolicy::renderer_owned, record.m_depth_texture->getDesc().format);
        }
    }

    std::error_code Renderer::createSurfaceDepth_(const int2 &extent, rhi::ComPtr<rhi::ITexture> &out_texture,
                                                  rhi::ComPtr<rhi::ITextureView> &out_view) const {
        rhi::TextureDesc depth_desc{};
        depth_desc.type = rhi::TextureType::Texture2D;
        depth_desc.size = {safe_narrowing<u32>(extent.x), safe_narrowing<u32>(extent.y), 1u};
        depth_desc.arrayLength = 1u;
        depth_desc.mipCount = 1u;
        depth_desc.format = rhi::Format::D32Float;
        depth_desc.sampleCount = 1u;
        depth_desc.memoryType = rhi::MemoryType::DeviceLocal;
        depth_desc.usage = rhi::TextureUsage::DepthStencil;
        depth_desc.defaultState = rhi::ResourceState::DepthWrite;
        depth_desc.label = "window surface depth";

        PPR_RETURN_ERROR_ON_FAIL(Renderer,
            m_rhi_service->getDevice().createTexture(depth_desc, nullptr, out_texture.writeRef()));
        out_view = out_texture->getDefaultView();
        if (not out_view) [[unlikely]] {
            out_texture.setNull();
            return make_error_code(std::errc::resource_unavailable_try_again);
        }
        return default_value_v;
    }

    std::error_code Renderer::destroyWindowSurface_(const WindowHandle window_handle) {
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        const auto it = m_surfaces.find(window_handle);
        if (it == m_surfaces.end()) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        SurfaceRecord record = std::move(it->second);
        m_surfaces.erase(it);

        std::error_code first_error{};
        if (record.m_configured) {
            PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_error, waitOnHost());
        }
        if (record.m_configured and record.m_surface) {
            PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_error, record.m_surface->unconfigure());
            record.m_configured = false;
        }

        record.m_depth_view.setNull();
        record.m_depth_texture.setNull();
        record.m_surface.setNull();
        return first_error;
    }
}
