module;
#include "pP/Macros.h"
module engine.app;

import :frame.packet;
import :renderer;
import :renderer.frame_fence;
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

    namespace {
        // ora-25 measurement-only probe writer: acquire/submit/present and
        // fence retirement stamp SEPARATELY with outcomes + fence values.
        // Runs on the owner thread (lane worker, or main when inline), so
        // it writes the spinlock-guarded lane buffer main merges after
        // join. Epoch/frame keys are unavailable here (no reservation
        // context); correlation rides cpu ticks + GPU fence values.
        // noexcept, allocation-free, never touches control flow.
        void recordRendererTelemetry_(
            const EFrameTelemetryEvent kind, const int host_outcome = 0, const i64 detail_a = 0, const i64 detail_b = 0,
            const FenceCompletionValue gpu_fence = kFenceValueNone) noexcept {
            FrameTelemetryEvent event{};
            event.m_kind = kind;
            event.m_cpu_tick_ns = frameTelemetryCpuTickNs(ITimerClock::steady().now());
            event.m_gpu_fence_value = gpu_fence;
            event.m_detail_a = detail_a;
            event.m_detail_b = detail_b;
            event.m_host_outcome = host_outcome;
            event.m_thread_tag = 1u;
            frameTelemetryRecordLane(event);
        }
    }

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
            rhi::ITextureView *const view, AttachmentInfo *const out_info) noexcept {
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
            const u32 mip_span = range.mipCount == kAllRemaining ? texture_desc.mipCount - range.mip : range.mipCount;
            const u32 layer_span = range.layerCount == kAllRemaining ? texture_desc.arrayLength - range.layer
                                                                     : range.layerCount;
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

            const rhi::Format format = view_desc.format == rhi::Format::Undefined ? texture_desc.format
                                                                                  : view_desc.format;

            if (format == rhi::Format::Undefined or texture_desc.sampleCount == 0u) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            *out_info = AttachmentInfo{
                .m_format = format,
                .m_extent =
                    int2{
                        safe_narrowing(width),
                        safe_narrowing(height),
                    },
                .m_sample_count = texture_desc.sampleCount,
            };
            return default_value_v;
        }

        [[nodiscard]] std::error_code validateMatchingAttachment_(
            const AttachmentInfo &reference, const AttachmentInfo &candidate) noexcept {
            if (reference.m_extent.x != candidate.m_extent.x or
                reference.m_extent.y != candidate.m_extent.y or
                reference.m_sample_count != candidate.m_sample_count) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }
            return default_value_v;
        }

        void applyColorAttachmentOps_(
            rhi::RenderPassColorAttachment &attachment, const ColorAttachmentOps &ops) noexcept {
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
            const rhi::RenderPassDesc &render_pass, RenderPipelineSignature &out_signature, int2 &out_target_extent) {
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
                PPR_RETURN_ERROR_ON_FAIL(
                    Renderer, inspectAttachment_(render_pass.depthStencilAttachment->view, &depth_stencil_info));

                PPR_RETURN_ERROR_ON_FAIL(Renderer, validate_attachment(depth_stencil_info));
                signature.m_depth_stencil_format = depth_stencil_info.m_format;
            }

            if (not reference_attachment.has_value()) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            // The only u32 -> SampleCount seam: a non-power-of-two backbuffer
            // or attachment count fails closed here, before the signature is
            // published to any cache or pipeline.
            const Expected<SampleCount> sample_count = makeSampleCount(reference_attachment->m_sample_count);
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
            rhi::ICommandEncoder &encoder, string_literal description, const rhi::RenderPassDesc &render_pass,
            const RenderPipelineSignature &pipeline_signature, const int2 &target_extent,
            const std::span<const DrawSubmission> draws, rhi::IDevice &device,
            const FenceCompletionValue completed_fence_value,
            const std23::function_ref<std::error_code(FenceCompletionValue)> wait_for_fence) {
            rhi::IRenderPassEncoder *const pass = encoder.beginRenderPass(render_pass);
            if (pass == nullptr) [[unlikely]] {
                PPR_LOG(
                    Renderer, error, "beginRenderPass returned null",
                    {
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
                    static_cast<float>(target_extent.x), static_cast<float>(target_extent.y));
                const rhi::ScissorRect default_scissor = rhi::ScissorRect::fromSize(
                    safe_narrowing(target_extent.x), safe_narrowing(target_extent.y));

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

                    PPR_RETURN_ERROR_ON_FAIL(
                        Renderer, submission.m_encode_draws(
                                      DrawContext{
                                          .m_device = device,
                                          .m_pass = *pass,
                                          .m_render_pipeline_key = signature,
                                          .m_viewport = viewport,
                                          .m_scissor = scissor,
                                          .m_target_extent = target_extent,
                                          .m_completed_fence_value = completed_fence_value,
                                          .m_wait_for_fence = wait_for_fence,
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
        const hal::ProfileScope scope{"App.Renderer.Initialize"};
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        rhi::ComPtr<rhi::ICommandQueue> queue;
        rhi::IDevice &device = rhi_service.getDevice();
        PPR_RETURN_ERROR_ON_FAIL(Renderer, device.getQueue(rhi::QueueType::Graphics, queue.writeRef()));

        m_rhi_service = safe_ptr{&rhi_service};
        m_graphics_queue = std::move(queue);
        m_owner = std::this_thread::get_id();

        // The one app-owned monotonic fence: every frame SUBMIT signals the
        // next value through SubmitDesc signalFences. Created here so the
        // first submit already carries a value; never an internal queue ID.
        PPR_RETURN_ERROR_ON_FAIL(Renderer, m_frame_fence.initialize(device));

        m_submit_quarantine = false;
        m_injected_submit_failure = false;
        m_shutdown_complete = false;
        // Fresh retirement clock: never inherit a stale frontier from a
        // previous cycle of this Renderer.
        m_retirement_frontier = kFenceValueNone;
        m_last_submit_settlement = FrameSettlement::kRejected;

        // Bounded by the credit ledger: the unretired set never exceeds
        // kMaxOutstandingFrames, so this reserve keeps every steady-state
        // retireFrame_ below it allocation-free (retirement runs on scope
        // guards, including unwind paths).
        m_unretired_submits.reserve(kMaxOutstandingFrames);

        PPR_LOG(Renderer, info, "Renderer initialized");
        return default_value_v;
    }

    std::error_code Renderer::shutdown() {
        const hal::ProfileScope scope{"App.Renderer.Shutdown"};
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        PPR_LOG(
            Renderer, info, "Renderer shut down",
            {
                {"surfaces", m_surfaces.size()},
            });

        // Release only after a safe disposition: quiescence is established
        // WITHOUT releasing, and an UNKNOWN outcome retains every GPU object
        // (fence alive, deferred entries held, surfaces and queue untouched)
        // for process exit — the OS reclaims retained objects. Only proven
        // idle or positively-confirmed removal proceeds below.
        const GpuQuiescenceResult quiescence = establishGpuQuiescence();
        std::error_code first_err = quiescence.m_error;
        if (quiescence.m_disposition == GpuQuiescence::kUnknown) {
            m_submit_quarantine = true;
            return first_err;
        }

        // Shutdown under load: retirement is the inverse of submission.
        // Proven idle retires through the fence shutdown (which waits the
        // last submission only when one is outstanding — harmless after the
        // idle above). Positively-confirmed removal abandons wait-free and
        // releases without touching the device below. abandon() and
        // notifyDeviceLost() prove nothing by themselves and are never used
        // as proof on the UNKNOWN path (which returned above).
        if (quiescence.m_disposition == GpuQuiescence::kRemovalConfirmed) {
            m_submit_quarantine = true;
            // Confirmed removal releases without new device work: production queue submission after removal is
            // forbidden.
            m_frame_fence.abandon();
        } else {
            PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, m_frame_fence.shutdown());
            if (first_err) {
                const bool confirmed_removal = first_err ==
                                                   std::make_error_code(std::errc::no_such_device_or_address) or
                                               m_frame_fence.isTerminal();
                m_submit_quarantine = true;
                if (not confirmed_removal) {
                    return first_err;
                }
            }
        }
        sweepCompleted_();
        std::ignore = m_deferred.drainAll();
        m_unretired_submits.clear();
        // The reset here happens AFTER GPU quiescence was proven or loss was
        // confirmed, so the frontier resets to "no proven retirement" for a
        // fresh start — never to an invented value.
        m_retirement_frontier = kFenceValueNone;
        m_last_submit_settlement = FrameSettlement::kRejected;
        m_credits.reset();
        m_submit_quarantine = false;
        m_injected_submit_failure = false;
        m_injected_quiescence_failure = std::error_code{};
        m_test_hold_sweep = false;
        // Mark retirement closed only AFTER every fence/credit/surface
        // resource was released above: post-shutdown sweeps (the lane's
        // final drain runs after this via stopRenderLane_()) then observe
        // a clean zero instead of the released fence's not_connected.
        m_shutdown_complete = true;

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

    void Renderer::notifyDeviceLost() noexcept {
        // GPU objects are gone (positively-confirmed removal ONLY — never a
        // generic UNKNOWN wait failure, which must retain instead): nothing in flight is worth waiting for.
        // Discard pending retirements outright (mirroring the pass-owned
        // cache contract), drop credits without completing them, and abandon
        // the fence — abandon(), never shutdown(), because shutdown() is
        // allowed to wait and this hook must stay wait-free. Surfaces and
        // the queue release below without touching the device; restart is
        // an explicit shutdown + initialize pair. Cancellation wakes waits,
        // it does not interrupt a driver present, so this hook never blocks
        // on the GPU.
        std::ignore = m_deferred.drainAll();
        m_unretired_submits.clear();
        m_retirement_frontier = kFenceValueNone;
        m_last_submit_settlement = FrameSettlement::kRejected;
        m_credits.reset();
        m_submit_quarantine = false;
        m_injected_submit_failure = false;
        m_injected_quiescence_failure = std::error_code{};
        m_test_hold_sweep = false;
        m_frame_fence.abandon();
        for (auto entry: m_surfaces) {
            SurfaceRecord &record = entry.second;
            record.m_configured = false;
            record.m_depth_view.setNull();
            record.m_depth_texture.setNull();
            record.m_surface.setNull();
        }
        m_surfaces.clear();
        m_graphics_queue.setNull();
    }

    void Renderer::quarantineAdmission() noexcept {
        m_submit_quarantine = true;
    }

    void Renderer::setTestHoldSweep(const bool hold) noexcept {
        m_test_hold_sweep = hold;
    }

    void Renderer::driveQueueRetirementForTest() noexcept {
        // Backend retirement workaround, relocated from the production
        // notifyDeviceLost path (removed: reference speculation on a loss
        // hook): slang-rhi keeps submitted command buffers parked in the
        // queue's m_commandBuffersInFlight until a submit-time poll (or a
        // waitOnHost()) observes completion. Tests that inject loss AFTER
        // proven completion submit an empty submission purely to drive
        // that poll, on the owner thread, with every real value already
        // retired. Never called from a production path.
        if (m_graphics_queue) {
            rhi::SubmitDesc empty{};
            std::ignore = make_error_code(m_graphics_queue->submit(empty));
        }
    }

    void Renderer::sweepCompleted_() noexcept {
        // Tests-only delayed-completion seam: while held, real submits stay
        // outstanding (no reclaim, no credit return) so the test can assert
        // two TRULY PENDING submissions via real fence values.
        if (m_test_hold_sweep) {
            return;
        }
        const Expected<FenceCompletionValue> completed = m_frame_fence.pollCompleted();
        const FenceCompletionValue known = completed.has_value() ? *completed : m_frame_fence.completed();
        std::ignore = m_deferred.reclaim(known);
        std::size_t kept = 0u;
        // Frontier is the maximum of values whose entries actually left the
        // set below — proven by completion, never invented by poll failure,
        // a hold seam, or a removal.
        FenceCompletionValue retired_max = m_retirement_frontier;
        for (std::size_t index = 0u; index < m_unretired_submits.size(); ++index) {
            if (m_unretired_submits[index] <= known) {
                // Exactly one credit return per submitted value: the value
                // leaves the set here, so it can never return twice. A
                // failed poll keeps every value (no invented completion).
                std::ignore = m_credits.release();
                retired_max = std::max(retired_max, m_unretired_submits[index]);
            } else {
                if (kept != index) {
                    m_unretired_submits[kept] = m_unretired_submits[index];
                }
                ++kept;
            }
        }
        m_unretired_submits.resize(kept);
        m_retirement_frontier = retired_max;
    }

    void Renderer::retireFrame_(const FenceCompletionValue submitted_value, const FrameSettlement settlement) noexcept {
        m_last_submit_settlement = settlement;
        switch (settlement) {
            case FrameSettlement::kSubmitted:
                // The credit travels with the submission: it returns in a
                // later sweep once the fence passes this value (often the
                // very next sweep below, since present already throttled the
                // frame). The reserve in initialize() keeps this push
                // allocation-free within the credit bound.
                m_unretired_submits.push_back(submitted_value);
                break;
            case FrameSettlement::kSubmitFailed:
                // Possibly executed: stop admission and HOLD the credit
                // (never returned early, never reused) until shutdown
                // proves idle or notifyDeviceLost confirms the loss. The
                // signal value was never committed, so no ambiguous value
                // can ever be waited on or recycled.
                m_submit_quarantine = true;
                break;
            case FrameSettlement::kRejected:
                // Definitely unsubmitted: nothing references this frame's
                // writes, so the credit returns immediately — still exactly
                // once per admitted frame.
                std::ignore = m_credits.release();
                break;
        }
        sweepCompleted_();
    }

    std::error_code Renderer::admitFrame_() {
        // Nonblocking admission: probe (quarantine/terminal/sweep/capacity
        // with distinct codes) without taking anything, then take one
        // credit. Single lane, so nothing interleaves between probe and
        // acquire: a successful probe always has a credit below.
        if (const std::error_code probe_err = checkFrameAdmission()) [[unlikely]] {
            return probe_err;
        }
        return m_credits.acquire();
    }

    std::error_code Renderer::checkFrameAdmission() {
        // A quarantined submit failure parks the pipeline until teardown
        // settles it (proven idle or confirmed loss): parked admission is a
        // teardown/recovery signal, never capacity backpressure.
        if (m_submit_quarantine) [[unlikely]] {
            return std::make_error_code(std::errc::state_not_recoverable);
        }
        if (m_frame_fence.isTerminal()) [[unlikely]] {
            return std::make_error_code(std::errc::no_such_device_or_address);
        }
        // Sweep completions first so a finished frame never squats a credit.
        sweepCompleted_();
        // Recheck AFTER sweeping: the sweep's poll may have observed the
        // removal sentinel and latched terminal mid-sweep — never admit
        // past it. Only capacity-busy survives as the skippable outcome.
        if (m_frame_fence.isTerminal()) [[unlikely]] {
            return std::make_error_code(std::errc::no_such_device_or_address);
        }
        if (m_credits.outstanding() >= kMaxOutstandingFrames) [[unlikely]] {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        return default_value_v;
    }

    void Renderer::setInjectedSubmitFailure(const bool fail) noexcept {
        m_injected_submit_failure = fail;
    }

    Expected<FenceCompletionValue> Renderer::submitAndSignal_(rhi::ICommandBuffer *const command_buffer) {
        if (not m_frame_fence.isInitialized() or not m_graphics_queue or command_buffer == nullptr) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        // After-execute placement on the single graphics queue: the value is
        // signalled once this submission's work completes. The value is
        // committed ONLY on success — a failed submit burns the internal
        // queue ID at most, never the app-owned monotonic sequence. A
        // failed submit may still have executed, so the caller must
        // quarantine the frame (never a normal release).
        const FenceCompletionValue value = m_frame_fence.nextValue();
        if (m_injected_submit_failure) [[unlikely]] {
            // Gate 1 determinism seam (tests only): simulates the
            // device-removed class where the signal may have failed AFTER
            // execute. No backend work runs and no value is committed.
            return std::unexpected{std::make_error_code(std::errc::io_error)};
        }
        rhi::ICommandBuffer *command_buffers[] = {command_buffer};
        rhi::IFence *signal_fences[] = {m_frame_fence.fence()};
        const u64 signal_values[] = {value};
        rhi::SubmitDesc submit_desc{};
        submit_desc.commandBuffers = command_buffers;
        submit_desc.commandBufferCount = 1u;
        submit_desc.signalFences = signal_fences;
        submit_desc.signalFenceValues = signal_values;
        submit_desc.signalFenceCount = 1u;
        if (const std::error_code err = make_error_code(m_graphics_queue->submit(submit_desc))) [[unlikely]] {
            return std::unexpected{err};
        }
        m_frame_fence.commitSubmitted(value);
        return value;
    }

    std::error_code Renderer::waitForFenceValue(const FenceCompletionValue value) {
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        return m_frame_fence.waitFor(value);
    }

    std::error_code Renderer::retireCompletedFrames() {
        const hal::ProfileScope scope_frame_retire{"PPR.Frame.Retire"};
        // Post-shutdown: the fence/queue/service are released, so a poll
        // would report not_connected. Retirement is DONE at that point —
        // report the proven clean state, never an invented failure. Real
        // poll failures still surface while the renderer is live.
        if (m_shutdown_complete) [[unlikely]] {
            return default_value_v;
        }
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        sweepCompleted_();
        const Expected<FenceCompletionValue> retired = m_frame_fence.pollCompleted();
        // ora-25: fence-retirement stamp with the proven frontier (GPU
        // domain, m_gpu_fence_value) plus outstanding count, separate from
        // submit/present stamps; poll failures carry the host outcome.
        recordRendererTelemetry_(
            EFrameTelemetryEvent::fence_retired, retired ? 0 : retired.error().value(),
            static_cast<i64>(m_retirement_frontier), static_cast<i64>(m_credits.outstanding()), m_retirement_frontier);
        if (not retired) {
            return retired.error();
        }
        return default_value_v;
    }

    FenceCompletionValue Renderer::lastSubmittedFenceValue() const noexcept {
        return m_frame_fence.lastSubmitted();
    }

    FenceCompletionValue Renderer::completedFenceValue() const noexcept {
        return m_frame_fence.completed();
    }

    u32 Renderer::outstandingFrameCount() const noexcept {
        return m_credits.outstanding();
    }

    SurfaceGeneration Renderer::surfaceGenerationFor(const WindowHandle window_handle) const noexcept {
        const auto it = m_surfaces.find(window_handle);
        if (it == m_surfaces.end()) {
            return kSurfaceGenerationNone;
        }
        return it->second.m_generation;
    }

    bool Renderer::isSurfaceWorkObsolete(const SurfaceIdentity identity) const noexcept {
        const auto it = m_surfaces.find(identity.m_handle);
        if (it == m_surfaces.end()) {
            return false;
        }
        return ::pP::isSurfaceWorkObsolete(true, identity.m_generation, it->second.m_generation);
    }

    std::error_code Renderer::waitOnHost() {
        const hal::ProfileScope scope{"App.Renderer.WaitOnHost", hal::profile_blocking_category};
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        if (m_graphics_queue) {
            PPR_RETURN_ERROR_ON_FAIL(Renderer, m_graphics_queue->waitOnHost());
        }
        return default_value_v;
    }

    GpuQuiescenceResult Renderer::establishGpuQuiescence() {
        // Tests-only injected outcome: classified exactly like a backend
        // wait with the same error (a healthy fence + generic failure is
        // UNKNOWN without removal marking). Never touches the backend.
        if (m_injected_quiescence_failure) {
            const std::error_code injected = m_injected_quiescence_failure;
            const bool confirmed_removal = injected == std::make_error_code(std::errc::no_such_device_or_address) or
                                           m_frame_fence.isTerminal();
            return GpuQuiescenceResult{
                .m_disposition = confirmed_removal ? GpuQuiescence::kRemovalConfirmed : GpuQuiescence::kUnknown,
                .m_error = injected,
            };
        }
        if (const std::error_code wait_err = waitOnHost()) {
            // A failed host wait is UNKNOWN completion, never confirmed
            // loss — except a positively-confirmed removal code (or a fence
            // that already latched terminal, e.g. via the removal sentinel
            // observed mid-frame). Nothing is released or reused here.
            const bool confirmed_removal = wait_err == std::make_error_code(std::errc::no_such_device_or_address) or
                                           m_frame_fence.isTerminal();
            return GpuQuiescenceResult{
                .m_disposition = confirmed_removal ? GpuQuiescence::kRemovalConfirmed : GpuQuiescence::kUnknown,
                .m_error = wait_err,
            };
        }
        return GpuQuiescenceResult{.m_disposition = GpuQuiescence::kIdleProven, .m_error = default_value_v};
    }

    void Renderer::setInjectedQuiescenceFailureForTest(const std::error_code error) noexcept {
        m_injected_quiescence_failure = error;
    }

    rhi::ICommandQueue *Renderer::graphicsQueueForTest() const noexcept {
        return m_graphics_queue.get();
    }

    std::error_code Renderer::render(
        string_literal description, const rhi::RenderPassDesc &render_pass,
        const std::initializer_list<DrawSubmission> draws) {
        const hal::ProfileScope scope{"App.Renderer.Render"};
        // Off-thread (affinity) rejection: definitely unsubmitted, no credit
        // taken — and owner-confined state untouched. The settlement member
        // stays whatever the owner thread last stamped (this wrong-thread
        // call must never race it); the caller classifies this invocation
        // as definitely-rejected from the returned error alone.
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        return renderDraws_(description, render_pass, std::span{draws.begin(), draws.size()});
    }

    std::error_code Renderer::renderDraws_(
        string_literal description, const rhi::RenderPassDesc &render_pass,
        const std::span<const DrawSubmission> draws) {
        const hal::ProfileScope scope_profile{"App.Renderer.Draws"};
        // Pre-guard rejections (F1 stale fate): definitely unsubmitted, no
        // credit taken — stamp kRejected so this call never inherits the
        // previous frame's settlement. Only admitted frames ride the
        // retireFrame_ guard below.
        if (not m_graphics_queue or not m_rhi_service.isValid()) [[unlikely]] {
            m_last_submit_settlement = FrameSettlement::kRejected;
            return std::make_error_code(std::errc::not_connected);
        }

        // Nonblocking admission BEFORE construction: sweep completions
        // first so a finished frame never squats a credit, then take one.
        // Capacity-busy fails the frame closed WITHOUT building anything
        // (single lane: the caller retries with a fresh frame; the Editor
        // maps only capacity-busy to a dropped frame, never an app
        // failure). Quarantine/terminal failures carry their distinct codes
        // to teardown/recovery — never suppressed as dropped frames.
        // Exactly one settlement per admitted frame rides the defer below.
        if (const std::error_code err = admitFrame_()) [[unlikely]] {
            m_last_submit_settlement = FrameSettlement::kRejected;
            return err;
        }
        FenceCompletionValue submitted_value{kFenceValueNone};
        FrameSettlement settlement{FrameSettlement::kRejected};
        PPR_DEFER {
            retireFrame_(submitted_value, settlement);
        };

        const auto scope = mem::ScratchPad::open();

        RenderPipelineSignature pipeline_signature{};
        int2 target_extent{zero_v};
        if (const std::error_code signature_err = buildRenderSignature_(render_pass, pipeline_signature, target_extent))
            [[unlikely]] {
            return signature_err;
        }

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, m_graphics_queue->createCommandEncoder(encoder.writeRef()));

        const FenceCompletionValue completed = m_frame_fence.completed();
        const auto wait_for_fence = [this](const FenceCompletionValue value) -> std::error_code {
            return m_frame_fence.waitFor(value);
        };
        if (const std::error_code encode_err = encodeRenderPass_(
                *encoder, description, render_pass, pipeline_signature, target_extent, draws,
                m_rhi_service->getDevice(), completed, wait_for_fence)) [[unlikely]] {
            return encode_err;
        }

        rhi::ComPtr<rhi::ICommandBuffer> command_buffer;
        PPR_RETURN_ERROR_ON_FAIL(Renderer, encoder->finish(command_buffer.writeRef()));
        const Expected<FenceCompletionValue> submitted = submitAndSignal_(command_buffer.get());
        if (not submitted) [[unlikely]] {
            // Possibly executed: quarantine (admission stops, credit held)
            // rather than a normal release. The value was never committed,
            // so no ambiguous value can be reused.
            settlement = FrameSettlement::kSubmitFailed;
            return submitted.error();
        }
        submitted_value = *submitted;
        settlement = FrameSettlement::kSubmitted;
        return default_value_v;
    }

    std::error_code Renderer::renderToTexture(
        rhi::ITexture &render_target, const std::initializer_list<DrawSubmission> draws,
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
        return render(
            description,
            rhi::RenderPassDesc{
                .colorAttachments = &color_attachment,
                .colorAttachmentCount = 1u,
            },
            draws);
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
        SurfaceRecord &record, const SurfaceRenderPass &surface_pass, const rhi::RenderPassDesc &render_pass,
        RenderPipelineSignature &out_signature, int2 &out_target_extent) {
        RenderPipelineSignature validated{};
        int2 validated_extent{zero_v};
        PPR_RETURN_ERROR_ON_FAIL(Renderer, buildRenderSignature_(render_pass, validated, validated_extent));

        SurfaceSignatureCache *cache = nullptr;
        if (surface_pass.m_depth_policy != ESurfaceDepthPolicy::external and surface_pass.m_additional_colors.empty()) {
            cache = std::addressof(record.m_signatures[static_cast<std::size_t>(surface_pass.m_depth_policy)]);
        }

        const bool reusable = cache != nullptr and
                              cache->m_valid and
                              cache->m_depth_policy == surface_pass.m_depth_policy and
                              cache->m_extent.x == record.m_extent.x and
                              cache->m_extent.y == record.m_extent.y and
                              cache->m_extent.x == validated_extent.x and
                              cache->m_extent.y == validated_extent.y and
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
        const Window &window, const std::initializer_list<SurfaceRenderPass> passes) {
        return renderAndPresentImpl_(
            SurfaceWindowSnapshot{
                .m_handle = window.m_handle,
                .m_native = window.m_native,
                .m_framebuffer_size = window.m_framebuffer_size,
                .m_title = window.m_title,
            },
            std::span<const SurfaceRenderPass>{passes.begin(), passes.size()},
            SurfaceIdentity{.m_handle = window.m_handle, .m_generation = kSurfaceGenerationNone});
    }

    std::error_code Renderer::renderAndPresent(
        const Window &window, const std::initializer_list<SurfaceRenderPass> passes, const SurfaceIdentity identity) {
        return renderAndPresentImpl_(
            SurfaceWindowSnapshot{
                .m_handle = window.m_handle,
                .m_native = window.m_native,
                .m_framebuffer_size = window.m_framebuffer_size,
                .m_title = window.m_title,
            },
            std::span<const SurfaceRenderPass>{passes.begin(), passes.size()}, identity);
    }

    std::error_code Renderer::renderAndPresent(
        const SurfaceWindowSnapshot &window, const std::span<const SurfaceRenderPass> passes,
        const SurfaceIdentity identity) {
        return renderAndPresentImpl_(window, passes, identity);
    }

    std::error_code Renderer::renderAndPresentImpl_(
        const SurfaceWindowSnapshot &window, const std::span<const SurfaceRenderPass> passes,
        const SurfaceIdentity identity) {
        const hal::ProfileScope scope{"App.Renderer.Present"};
        // Pre-guard rejections (F1 stale fate): definitely unsubmitted, no
        // credit taken — stamp kRejected so THIS invocation never inherits
        // the previous frame's settlement. Only admitted frames ride the
        // retireFrame_ guard below. The off-thread (affinity) reject above
        // is the exception: it leaves the owner-confined settlement
        // untouched (never races an accepted fate the owner stamped) and the
        // caller classifies it from the returned error alone.
        if (m_rhi_service.isValid() and not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (not m_graphics_queue or not m_rhi_service.isValid()) [[unlikely]] {
            m_last_submit_settlement = FrameSettlement::kRejected;
            return std::make_error_code(std::errc::not_connected);
        }
        if (not window.m_handle) [[unlikely]] {
            m_last_submit_settlement = FrameSettlement::kRejected;
            return std::make_error_code(std::errc::no_such_device_or_address);
        }
        if (identity.m_handle and identity.m_handle != window.m_handle) [[unlikely]] {
            m_last_submit_settlement = FrameSettlement::kRejected;
            return std::make_error_code(std::errc::invalid_argument);
        }

        // Nonblocking admission BEFORE any backend work: capacity-busy
        // fails the frame closed with no packet consumed and no surface
        // touched, while quarantine/terminal failures propagate under their
        // distinct codes. Exactly one settlement per admitted frame rides
        // the defer below, including every rejection and failure path.
        if (const std::error_code err = admitFrame_()) [[unlikely]] {
            m_last_submit_settlement = FrameSettlement::kRejected;
            return err;
        }
        FenceCompletionValue submitted_value{kFenceValueNone};
        FrameSettlement settlement{FrameSettlement::kRejected};
        PPR_DEFER {
            retireFrame_(submitted_value, settlement);
        };

        // Generational gate against the EXISTING record, BEFORE surface
        // control: the incoming claim is checked before any reconfiguration.
        // A mismatch that predates this frame retires as obsolete with no
        // backend work and no reconfigure. Only a claim that matched the
        // pre-frame generation (or no claim, or no record yet) proceeds; a
        // resize below then mints G+1 for this same frame, which proceeds by
        // construction — a packet is never reconfigured and then rejected
        // for that same reconfigure.
        {
            const auto existing = m_surfaces.find(window.m_handle);
            if (existing != m_surfaces.end() and
                identity.m_generation != kSurfaceGenerationNone and
                ::pP::isSurfaceWorkObsolete(true, identity.m_generation, existing->second.m_generation)) [[unlikely]] {
                PPR_LOG(
                    Renderer, info, "rejecting obsolete surface work before reconfiguration",
                    {
                        {"packet_generation", identity.m_generation},
                        {"record_generation", existing->second.m_generation},
                    });
                return std::make_error_code(std::errc::resource_unavailable_try_again);
            }
        }

        // Surface control is reconciled BEFORE acquisition (never after):
        // create/resize mints a fresh generation and revalidates every
        // signature against the new shape, so the current packet proceeds
        // against the surface it just shaped. The gate above already retired
        // every pre-existing mismatch, so no second check is needed below.
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

        // m_configured is the in-flight-work token: no claim means nothing was submitted, so restore may skip the
        // fence.
        if (not PPR_ENSURE(record->m_configured)) {
            return make_error_code(std::errc::resource_unavailable_try_again);
        }

        if (passes.empty()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        for (const SurfaceRenderPass &surface_pass: passes) {
            const bool invalid_depth_policy =
                (surface_pass.m_depth_policy == ESurfaceDepthPolicy::none and surface_pass.m_depth_stencil) or
                (surface_pass.m_depth_policy == ESurfaceDepthPolicy::renderer_owned and
                    (surface_pass.m_depth_stencil or not record->m_depth_texture or not record->m_depth_view)) or
                (surface_pass.m_depth_policy == ESurfaceDepthPolicy::external and not surface_pass.m_depth_stencil);
            if (invalid_depth_policy) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }
        }

        rhi::ComPtr<rhi::ITexture> image;
        {
            const hal::ProfileScope scope_acquire{"App.Renderer.Acquire"};
            // ora-25: acquire stamped SEPARATELY with its outcome (control
            // flow unchanged: the hoisted error takes the same macro path).
            const std::error_code acquire_err = make_error_code(record->m_surface->acquireNextImage(image.writeRef()));
            recordRendererTelemetry_(EFrameTelemetryEvent::renderer_acquire, acquire_err ? acquire_err.value() : 0);
            PPR_RETURN_ERROR_ON_FAIL(Renderer, acquire_err);
        }

        // Acquired-image settlement guard, armed IMMEDIATELY post-acquire:
        // every failure below (error code or exception from allocations
        // and draw callbacks) still settles the acquired image exactly
        // once. The normal path settles explicitly through settlePresent();
        // any unwind settles best-effort in the destructor (which never
        // throws and never masks the original failure). The guard borrows
        // the surface — the record outlives this call, and no map mutation
        // below can reseat it.
        struct AcquiredImageGuard {
            rhi::ISurface *m_surface{nullptr};
            bool m_settled{false};

            [[nodiscard]] std::error_code settlePresent() noexcept {
                if (m_settled or m_surface == nullptr) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                m_settled = true;
                return make_error_code(m_surface->present());
            }

            ~AcquiredImageGuard() noexcept {
                if (not m_settled and m_surface != nullptr) {
                    try {
                        std::ignore = m_surface->present();
                    } catch (...) {
                    }
                }
            }
        };
        AcquiredImageGuard acquired_guard{record->m_surface.get()};

        // From here the acquired image owns a backend obligation:
        // acquire ⇒ present unconditionally. Every failure below records
        // first_err and skips forward work, but the present at the end
        // still runs (the debug layer enforces acquire→present pairing;
        // both backends deadlock the next acquire otherwise).
        std::error_code first_err{};
        const rhi::ComPtr<rhi::ITextureView> image_view = image->getDefaultView();

        // Fail early when the acquired backbuffer no longer matches the
        // resize-matched depth view instead of tripping per-frame validation.
        // Only passes selecting renderer-owned depth need the pair to match.
        // An extent mismatch no longer returns early: it records the error
        // (which skips encode/submit below) and flows into the
        // release/present path — acquire ⇒ present unconditionally.
        {
            bool wants_owned_depth = false;
            for (const SurfaceRenderPass &surface_pass: passes) {
                if (surface_pass.m_depth_policy == ESurfaceDepthPolicy::renderer_owned) {
                    wants_owned_depth = true;
                    break;
                }
            }

            if (wants_owned_depth and not first_err) {
                AttachmentInfo acquired_info{};
                if (const std::error_code err = inspectAttachment_(image_view.get(), &acquired_info)) {
                    first_err = err;
                } else if (record->m_depth_view) {
                    AttachmentInfo depth_info{};
                    if (const std::error_code depth_err = inspectAttachment_(record->m_depth_view.get(), &depth_info)) {
                        first_err = depth_err;
                    } else if (acquired_info.m_extent.x != depth_info.m_extent.x or
                               acquired_info.m_extent.y != depth_info.m_extent.y) [[unlikely]] {
                        first_err = std::make_error_code(std::errc::invalid_argument);
                    }
                }
            }
        }

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
                case ESurfaceDepthPolicy::none: break;
                case ESurfaceDepthPolicy::renderer_owned:
                    built.m_depth_stencil = rhi::RenderPassDepthStencilAttachment{};
                    built.m_depth_stencil->view = record->m_depth_view.get();
                    break;
                case ESurfaceDepthPolicy::external: built.m_depth_stencil = *surface_pass.m_depth_stencil; break;
            }

            built.m_desc = rhi::RenderPassDesc{
                .colorAttachments = built.m_color_attachments.data(),
                .colorAttachmentCount = safe_narrowing(built.m_color_attachments.size()),
                .depthStencilAttachment = built.m_depth_stencil ? std::addressof(*built.m_depth_stencil) : nullptr,
            };
            built.m_draws = std::span<const DrawSubmission>{surface_pass.m_draws.begin(), surface_pass.m_draws.size()};

            first_err = surfaceSignature_(*record, surface_pass, built.m_desc, built.m_signature, built.m_extent);
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
        // Completion for ring-slot reuse is polled once at frame start and
        // handed to every pass; growth/slot gates wait through the fence.
        {
            const hal::ProfileScope scope_submit{"App.Renderer.Submit"};
            const FenceCompletionValue completed = m_frame_fence.completed();
            const auto wait_for_fence = [this](const FenceCompletionValue value) -> std::error_code {
                return m_frame_fence.waitFor(value);
            };
            // An extent mismatch recorded first_err above, so this block is
            // skipped and the frame becomes present-only: no encode, no
            // submit, but the acquired image still meets its obligation.
            if (not first_err) {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_RETAIN_ERROR_ON_FAIL(
                    Renderer, first_err, m_graphics_queue->createCommandEncoder(encoder.writeRef()));

                for (const BuiltSurfacePass &built: built_passes) {
                    if (first_err) {
                        break;
                    }
                    first_err = encodeRenderPass_(
                        *encoder, description, built.m_desc, built.m_signature, built.m_extent, built.m_draws,
                        m_rhi_service->getDevice(), completed, wait_for_fence);
                }

                if (not first_err) {
                    rhi::ComPtr<rhi::ICommandBuffer> command_buffer;
                    PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, encoder->finish(command_buffer.writeRef()));
                    if (not first_err) {
                        // One monotonic value per frame SUBMIT, signalled
                        // after execute. A failed submit commits no value
                        // but may still have executed: the frame quarantines
                        // (admission stops, credit held) instead of
                        // releasing normally. A present failure below leaves
                        // the accepted submission GPU-pending until
                        // completion (or the device-loss teardown path) —
                        // never recycled early.
                        const Expected<FenceCompletionValue> submitted = submitAndSignal_(command_buffer.get());
                        if (submitted) {
                            submitted_value = *submitted;
                            settlement = FrameSettlement::kSubmitted;
                        } else {
                            first_err = submitted.error();
                            settlement = FrameSettlement::kSubmitFailed;
                        }
                        // ora-25: submit stamped SEPARATELY with the committed
                        // fence value (GPU domain) or the failure outcome;
                        // a failed submit commits no value but may still have
                        // executed (quarantine path), so both ride the event.
                        recordRendererTelemetry_(
                            EFrameTelemetryEvent::renderer_submit, submitted ? 0 : submitted.error().value(), 0, 0,
                            submitted ? *submitted : kFenceValueNone);
                    }
                }
            }
        }

        // Submission acceptance is independent of present: the submit above
        // already committed its value (stamped by the caller even when the
        // present below fails), and the acquired image meets its
        // acquire ⇒ present obligation exactly once through the guard —
        // including the present-only path above.
        {
            const hal::ProfileScope scope_present{"App.Renderer.PresentFinal"};
            // ora-25: present stamped SEPARATELY with its outcome (control
            // flow unchanged: the hoisted error takes the same retain path).
            const std::error_code present_err = acquired_guard.settlePresent();
            recordRendererTelemetry_(
                EFrameTelemetryEvent::renderer_present, present_err ? present_err.value() : 0, 0, 0, submitted_value);
            PPR_RETAIN_ERROR_ON_FAIL(Renderer, first_err, present_err);
        }
        return first_err;
    }

    std::error_code Renderer::destroyWindowSurface(const Window &window) {
        if (not window.m_handle) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return destroyWindowSurface_(window.m_handle);
    }

    std::error_code Renderer::destroyWindowSurface(const WindowHandle window_handle) {
        if (not window_handle) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return destroyWindowSurface_(window_handle);
    }

    std::error_code Renderer::createWindowSurface_(const SurfaceWindowSnapshot &window, SurfaceRecord **out_record) {
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
        PPR_RETURN_ERROR_ON_FAIL(
            Renderer, device.createSurface(rhi::WindowHandle::fromHwnd(window.m_native), surface.writeRef()));

        SurfaceRecord record{};
        record.m_surface = std::move(surface);
        PPR_RETURN_ERROR_ON_FAIL(Renderer, resizeWindowSurface_(record, window.m_framebuffer_size));

        const auto [it, inserted] = m_surfaces.try_emplace(window.m_handle, std::move(record));
        if (not inserted) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        *out_record = std::addressof(it->second);

        PPR_LOG(
            Renderer, info, "window surface created",
            {
                {"description", window.m_title.data()},
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
            // Zero-size is an explicit shape change: mint a fresh generation
            // so packets built for the old shape are rejected before
            // acquisition instead of encoding against a dead surface.
            record.m_generation = ++m_surface_epoch;
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
        // A successful reconfigure is a new surface shape: mint a fresh
        // generation (existing waitOnHost ordering above is preserved — the
        // GPU is fenced before the surface is touched).
        record.m_generation = ++m_surface_epoch;

        // The surface shape is fixed by this configure and this depth texture,
        // so the cacheable signature entries are reseeded here — the only place
        // they can change — instead of being rebuilt on every frame.
        recomputeSurfaceSignatures_(record);

        PPR_LOG(
            Renderer, info, "window surface resized",
            {
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
        const rhi::Format surface_format = m_preferred_surface_format == rhi::Format::Undefined
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

    std::error_code Renderer::createSurfaceDepth_(
        const int2 &extent, rhi::ComPtr<rhi::ITexture> &out_texture, rhi::ComPtr<rhi::ITextureView> &out_view) const {
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

        PPR_RETURN_ERROR_ON_FAIL(
            Renderer, m_rhi_service->getDevice().createTexture(depth_desc, nullptr, out_texture.writeRef()));
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
