module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import engine.shader;
import engine.mesh;
import std;

namespace pP::tests::detail {
    namespace FrameFence {
        // ------------------------------------------------------------------
        // P1 fence-governed reuse: CPU-only rules first, GPU second.
        // The slot/ledger/deferral rules are pure CPU so the no-early-
        // overwrite proof runs with artificially delayed completion (fake
        // fence values), no device needed. GPU tests below prove two
        // outstanding signalled submissions complete in order on real
        // hardware, plus ring growth, atlas writes, and shutdown under load.
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (slot_selection_skips_incomplete_slots) {
            const std::array<FenceCompletionValue, 3u> values{5u, 6u, kFenceValueNone};
            PPR_TEST_ASSERT(not slotFenceComplete(4u, values[0]));
            PPR_TEST_ASSERT(not slotFenceComplete(4u, values[1]));
            PPR_TEST_ASSERT(slotFenceComplete(4u, values[2]));

            // Scan starts past the cursor: cursor 0 → slot 1 is busy, slot 2
            // is free, so slot 2 wins despite slot 0 (wrapping around) also
            // being busy — first complete slot in scan order.
            PPR_TEST_ASSERT(selectRingSlot(4u, values, 0u) == 2u);
            // Cursor 2 → slot 0 busy, slot 1 busy, slot 2 free → 2.
            PPR_TEST_ASSERT(selectRingSlot(4u, values, 2u) == 2u);
            // Completion catches up: every slot complete → next slot past
            // the cursor, preserving rotation.
            PPR_TEST_ASSERT(selectRingSlot(6u, values, 0u) == 1u);
            PPR_TEST_ASSERT(selectRingSlot(6u, values, 1u) == 2u);
        };

        PPR_UNIT_TEST (delayed_completion_never_overwrites_early) {
            // Artificially delayed completion: nothing completed, every slot
            // busy. Selection still returns a deterministic slot, but the
            // caller MUST wait — overwriting without the wait would corrupt
            // a GPU-read buffer.
            const std::array<FenceCompletionValue, 3u> values{7u, 8u, 9u};
            const u32 slot = selectRingSlot(6u, values, 0u);
            PPR_TEST_ASSERT(slot == 1u);
            PPR_TEST_ASSERT(not slotFenceComplete(6u, values[slot]));

            // Partial catch-up: slot 0 (7 <= 7) frees while 1 and 2 stay
            // busy. From cursor 0 the scan finds slot... order is 1, 2, 0:
            // 1 busy, 2 busy, 0 free → 0.
            PPR_TEST_ASSERT(selectRingSlot(7u, values, 0u) == 0u);
            // Full catch-up: pure rotation resumes.
            PPR_TEST_ASSERT(selectRingSlot(9u, values, 0u) == 1u);
        };

        PPR_UNIT_TEST (growth_gate_waits_for_youngest_use) {
            const std::array<FenceCompletionValue, 3u> values{3u, 9u, 5u};
            PPR_TEST_ASSERT(maxSlotValue(values) == 9u);

            const std::array<FenceCompletionValue, 3u> idle{kFenceValueNone, kFenceValueNone, kFenceValueNone};
            PPR_TEST_ASSERT(maxSlotValue(idle) == kFenceValueNone);
            PPR_TEST_ASSERT(maxSlotValue(std::span<const FenceCompletionValue>{}) == kFenceValueNone);
        };

        PPR_UNIT_TEST (ledger_binds_two_credits_then_busy) {
            FrameCreditLedger ledger{};
            PPR_TEST_ASSERT(ledger.capacity() == 2u);
            PPR_TEST_ASSERT(ledger.outstanding() == 0u);

            // Two outstanding GPU submissions: the eligible-workload bound.
            PPR_TEST_ASSERT(not ledger.acquire());
            PPR_TEST_ASSERT(not ledger.acquire());
            PPR_TEST_ASSERT(ledger.outstanding() == 2u);

            // Third admission fails closed — the Editor probes this bound
            // BEFORE building a packet and drops the frame on busy (never
            // an app failure); the Renderer re-checks at submit time.
            PPR_TEST_ASSERT(ledger.acquire() == std::make_error_code(std::errc::device_or_resource_busy));
            PPR_TEST_ASSERT(ledger.outstanding() == 2u);

            // Exactly one credit return per frame: each release frees one.
            PPR_TEST_ASSERT(not ledger.release());
            PPR_TEST_ASSERT(ledger.outstanding() == 1u);
            PPR_TEST_ASSERT(not ledger.acquire());
            PPR_TEST_ASSERT(ledger.outstanding() == 2u);

            // Double release past zero fails closed, never wraps.
            PPR_TEST_ASSERT(not ledger.release());
            PPR_TEST_ASSERT(not ledger.release());
            PPR_TEST_ASSERT(ledger.outstanding() == 0u);
            PPR_TEST_ASSERT(ledger.release() == std::make_error_code(std::errc::invalid_argument));

            // Device-loss escape: drops everything without completing it.
            PPR_TEST_ASSERT(not ledger.acquire());
            ledger.reset();
            PPR_TEST_ASSERT(ledger.outstanding() == 0u);

            // Code distinctness (G1-6): capacity-busy never shares a code
            // with the parked-pipeline outcomes (quarantine, terminal loss),
            // so producers can never mistake a parked pipeline for
            // backpressure all the way to teardown/recovery.
            PPR_TEST_ASSERT(
                std::make_error_code(std::errc::device_or_resource_busy) !=
                std::make_error_code(std::errc::state_not_recoverable));
            PPR_TEST_ASSERT(
                std::make_error_code(std::errc::device_or_resource_busy) !=
                std::make_error_code(std::errc::no_such_device_or_address));
        };

        PPR_UNIT_TEST (deferred_release_reclaims_only_completed) {
            DeferredReleaseQueue queue{};
            PPR_TEST_ASSERT(queue.pendingCount() == 0u);

            u32 destroyed = 0u;
            queue.defer(5u, [&destroyed]() noexcept { ++destroyed; });
            queue.defer(9u, [&destroyed]() noexcept { ++destroyed; });
            PPR_TEST_ASSERT(queue.pendingCount() == 2u);

            // Completion 6 retires only the first entry — the second entry's
            // objects may still be referenced, so they survive.
            PPR_TEST_ASSERT(queue.reclaim(6u) == 1u);
            PPR_TEST_ASSERT(destroyed == 1u);
            PPR_TEST_ASSERT(queue.pendingCount() == 1u);

            PPR_TEST_ASSERT(queue.reclaim(9u) == 1u);
            PPR_TEST_ASSERT(destroyed == 2u);
            PPR_TEST_ASSERT(queue.pendingCount() == 0u);

            // Shutdown/device-loss drain is unconditional.
            queue.defer(100u, [&destroyed]() noexcept { ++destroyed; });
            PPR_TEST_ASSERT(queue.drainAll() == 1u);
            PPR_TEST_ASSERT(destroyed == 3u);
        };

        PPR_UNIT_TEST (obsolete_surface_work_rejects_before_acquisition) {
            // Private by mandate: pure-CPU surface-identity rule, no device;
            // never enters any shared scope.
            // No record yet (first frame, lazily created surface): accept.
            PPR_TEST_ASSERT(not isSurfaceWorkObsolete(false, kSurfaceGenerationNone, kSurfaceGenerationNone));
            PPR_TEST_ASSERT(not isSurfaceWorkObsolete(false, 7u, 3u));
            // Live record, matching generation: accept.
            PPR_TEST_ASSERT(not isSurfaceWorkObsolete(true, 4u, 4u));
            // Live record, older packet (resize/minimize/recreate since
            // capture): reject before acquisition — no backend work.
            PPR_TEST_ASSERT(isSurfaceWorkObsolete(true, 4u, 5u));
            // A packet with no claim against a live record: reject (must
            // recapture; never encode blind against an unknown shape).
            PPR_TEST_ASSERT(isSurfaceWorkObsolete(true, kSurfaceGenerationNone, 1u));
        };

        PPR_UNIT_TEST (packet_carries_surface_identity) {
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};
            PPR_DEFER {
                std::ignore = window.release();
            };

            WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
            std::ignore = viewport.updateFromWindow();

            Camera camera{ECameraProjection::perspective};
            FramePacket packet = makeFramePacket(
                3u, TimeSpan{}, TimeSpan{}, camera.getSnapshot(), viewport.getViewport(),
                viewport.getViewportRevision(), window);
            PPR_TEST_ASSERT(packet.m_surface_identity == window.m_handle);
            PPR_TEST_ASSERT(packet.m_surface_generation == kSurfaceGenerationNone);
            PPR_TEST_ASSERT(hasRenderTarget(packet));

            // The producer stamps the observed generation after capture.
            packet.m_surface_generation = 11u;
            PPR_TEST_ASSERT(packet.m_surface_generation == 11u);
        };

        PPR_UNIT_TEST (failed_frames_return_their_credit_exactly_once) {
            // Private: uninitialized-Renderer fresh semantics with no device
            // booted at all; nothing to share.
            // Uninitialized renderer: every submission path fails before or
            // at admission, and each failure settles its lease — no credit
            // is held, none leaks, none returns twice.
            Renderer renderer{};
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == kFenceValueNone);
            PPR_TEST_ASSERT(renderer.completedFenceValue() == kFenceValueNone);
            PPR_TEST_ASSERT(
                renderer.surfaceGenerationFor(WindowHandle{reinterpret_cast<void *>(1)}) == kSurfaceGenerationNone);

            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{0, 0}, .m_window_size = int2{64, 64}}};
            window.m_framebuffer_size = int2{64, 64};
            PPR_DEFER {
                std::ignore = window.release();
            };

            // Present path fails with no queue; the credit is not consumed.
            const std::initializer_list<SurfaceRenderPass> no_passes{};
            PPR_TEST_ASSERT(renderer.renderAndPresent(window, no_passes) != std::error_code{});
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            // Generational overload settles the same way.
            PPR_TEST_ASSERT(
                renderer.renderAndPresent(
                    window, no_passes,
                    SurfaceIdentity{.m_handle = window.m_handle, .m_generation = kSurfaceGenerationNone}) !=
                std::error_code{});
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            // Uninitialized shutdown is a no-op success (idempotent).
            PPR_TEST_ASSERT(not renderer.shutdown());
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        PPR_UNIT_TEST (delayed_completion_retires_through_real_queues) {
            // Deterministic delayed completion through the REAL retirement
            // resource paths (ledger + deferred queue + slot helpers, no
            // device): two frames simultaneously pending, completion
            // arriving one value at a time. Fake fence values stand in for
            // the GPU; every accounting rule exercised is the production
            // one (sweep/reclaim/credit-return/slot-selection).
            FrameCreditLedger ledger{};
            DeferredReleaseQueue queue{};
            std::array<FenceCompletionValue, 3u> slots{kFenceValueNone, kFenceValueNone, kFenceValueNone};

            // Two simultaneously pending: both admitted, third refused.
            PPR_TEST_ASSERT(not ledger.acquire());
            PPR_TEST_ASSERT(not ledger.acquire());
            PPR_TEST_ASSERT(ledger.acquire() == std::make_error_code(std::errc::device_or_resource_busy));
            PPR_TEST_ASSERT(ledger.outstanding() == 2u);

            // Each frame stamps its ring slot and defers its releases at
            // its own submission value.
            constexpr FenceCompletionValue kFirst = 11u;
            constexpr FenceCompletionValue kSecond = 12u;
            slots[0] = kFirst;
            slots[1] = kSecond;
            u32 destroyed = 0u;
            queue.defer(kFirst, [&destroyed]() noexcept { ++destroyed; });
            queue.defer(kSecond, [&destroyed]() noexcept { ++destroyed; });

            // Nothing complete: no reclaim, no credit, every written slot
            // busy, selection falls through to the mandatory-wait slot.
            PPR_TEST_ASSERT(queue.reclaim(kFenceValueNone) == 0u);
            PPR_TEST_ASSERT(destroyed == 0u);
            PPR_TEST_ASSERT(not slotFenceComplete(kFenceValueNone, slots[0]));
            PPR_TEST_ASSERT(not slotFenceComplete(kFenceValueNone, slots[1]));
            PPR_TEST_ASSERT(slotFenceComplete(kFenceValueNone, slots[2]));

            // First value completes: exactly its release reclaims, exactly
            // one credit returns, its slot frees while the younger stays
            // busy (no early overwrite of the still-pending frame).
            PPR_TEST_ASSERT(queue.reclaim(kFirst) == 1u);
            PPR_TEST_ASSERT(destroyed == 1u);
            PPR_TEST_ASSERT(queue.pendingCount() == 1u);
            PPR_TEST_ASSERT(not ledger.release());
            PPR_TEST_ASSERT(ledger.outstanding() == 1u);
            PPR_TEST_ASSERT(slotFenceComplete(kFirst, slots[0]));
            PPR_TEST_ASSERT(not slotFenceComplete(kFirst, slots[1]));
            PPR_TEST_ASSERT(selectRingSlot(kFirst, slots, 1u) == 2u);

            // Second value completes: the remainder retires, the ledger
            // drains to zero — every credit exactly once.
            PPR_TEST_ASSERT(queue.reclaim(kSecond) == 1u);
            PPR_TEST_ASSERT(destroyed == 2u);
            PPR_TEST_ASSERT(queue.pendingCount() == 0u);
            PPR_TEST_ASSERT(not ledger.release());
            PPR_TEST_ASSERT(ledger.outstanding() == 0u);
            PPR_TEST_ASSERT(slotFenceComplete(kSecond, slots[1]));
        };

        // ------------------------------------------------------------------
        // GPU tests: real device, real fence, real submissions.
        // ------------------------------------------------------------------
        // Headless-proven in this file (real Renderer + real fence values):
        //   two_truly_pending_submissions_via_real_fence_values (B3: two
        //     accepted N/N+1 both uncompleted at assert time under the
        //     tests-only hold seam, then ordered completion);
        //   saturated_frame_skips_without_building_and_preserves_dt (B2c/B2a:
        //     saturated credits refuse busy, no packet built, success
        //     reported, skipped dt carried to the next admitted frame);
        //   loaded_editor_shutdown_under_pending_frames (B3: pending offscreen
        //     frames through the REAL ApplicationEditor::shutdown idle-first
        //     order); pass-owned half in pass_shutdown_after_established_idle;
        //   submission_acceptance_drives_pass_stamping (G1-5 headless half).
        // Explicitly-manual windowed items (no window exists in the test
        // environment; a live swapchain is required, so these stay Gate 1
        // manual requirements — never claimed as headless evidence):
        //   MANUAL_GATE1_RESIZE_THROUGH_EDITOR (windowed; production site:
        //     lib/engine/app/renderer/App.Renderer.cpp renderAndPresentImpl_
        //     generational gate + surface-control block, and
        //     lib/engine/app/App.Application.Editor.cpp renderPacket_
        //     obsolescence pre-check): drive a live window through resize +
        //     minimize-to-zero + restore + DPI change while presenting: the
        //     loop must never exit, the same-frame reconfigure must present
        //     (not self-reject), and a genuinely stale packet must drop as
        //     non-rendering success.
        //   MANUAL_GATE1_PRESENT_FAILURE_STAMP (windowed present half;
        //     production site: lib/engine/app/App.Application.Editor.cpp
        //     stampSubmittedPasses_ + renderPacket_ present section): an
        //     injected present failure on a live surface after an accepted
        //     SUBMIT: every participant (grid, triangle, fence-aware UI) is
        //     still stamped off the accepted value.
        //   MANUAL_GATE1_POST_ACQUIRE_UNWIND (windowed surface half;
        //     production site: lib/engine/app/renderer/App.Renderer.cpp
        //     renderAndPresentImpl_ AcquiredImageGuard): a throwing draw
        //     callback on a live surface: the acquired image still presents
        //     exactly once and the original exception propagates. The shared
        //     settlement unwind (exactly-once credit return) is proven
        //     headless in throwing_draw_callback_unwinds_exactly_once.
        //   MANUAL_GATE1_TWO_PENDING_PRESENT (windowed present half;
        //     production site: lib/engine/app/renderer/App.Renderer.cpp
        //     renderAndPresentImpl_ submit + present scopes): two GPU frames
        //     pending across present boundaries: queue depth vs host present
        //     vs GPU completion vs displayed cadence measured separately
        //     (completion is GPU-only; vsync throttles the host).

        constexpr ApplicationDomain kOffscreenDomain{
            .m_is_headless = true,
            .m_is_interactive = false,
            .m_needs_presence = false,
            .m_needs_rendering = true,
            .m_needs_user_interface = false,
        };

        struct OffscreenApp : Application {
            explicit OffscreenApp(const std::string_view name)
                : Application(kOffscreenDomain, name, std::span<const char *const>{}) {
            }

            [[nodiscard]] std::error_code boot() {
                return Application::initialize();
            }
            [[nodiscard]] std::error_code teardown() {
                return Application::shutdown();
            }
        };

        [[nodiscard]] CameraSnapshot orthoSnapshot800x600() {
            Camera cam;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            cam.updateModel(
                std::chrono::milliseconds{16}, model,
                Viewport{PixelRect{int2{0, 0}, int2{800, 600}}, ViewportLayout{}});
            return cam.getSnapshot();
        }

        [[nodiscard]] DrawSubmission submissionForTarget(auto &drawable, u32 extent) {
            DrawSubmission submission{drawable};
            submission.m_viewport = rhi::Viewport{
                .extentX = static_cast<float>(extent), .extentY = static_cast<float>(extent)};
            submission.m_scissor = rhi::ScissorRect{.maxX = extent, .maxY = extent};
            return submission;
        }

        [[nodiscard]] rhi::ComPtr<rhi::ITexture> makeOffscreenTarget(rhi::IDevice &device) {
            rhi::TextureDesc target_desc{};
            target_desc.type = rhi::TextureType::Texture2D;
            target_desc.size = {64u, 64u, 1u};
            target_desc.arrayLength = 1u;
            target_desc.mipCount = 1u;
            target_desc.format = rhi::Format::RGBA8Unorm;
            target_desc.sampleCount = 1u;
            target_desc.memoryType = rhi::MemoryType::DeviceLocal;
            target_desc.usage = rhi::TextureUsage::RenderTarget;
            target_desc.defaultState = rhi::ResourceState::RenderTarget;
            target_desc.label = "frame-fence test target";

            rhi::ComPtr<rhi::ITexture> target{};
            if (make_error_code(device.createTexture(target_desc, nullptr, target.writeRef()))) {
                return {};
            }
            return target;
        }

        namespace SharedFrameFenceGpu {
            // Shared process device for the frame_fence GPU leaves.
            //
            // Shape mirrors SharedWindowInputGpu (Holder + mutex + refcount +
            // acquire/release + service accessors); adapted: one OffscreenApp
            // is booted once (RHI + shader + content dir + skipped-dt carry),
            // while per-leaf Renderer/GridPass/TrianglePass/targets stay
            // leaf-local and tear down in inverse order before the group
            // release, so every safe_ptr dies before the device it observes.
            // RHI::initialize (RHI.cpp:235) and IShaderService::initialize
            // (Shader.cpp:170) are verified no-ops on the live pair, so the
            // per-leaf service-validity asserts are known-state gates, not
            // rebuilds; resetBetweenLeaves re-establishes the known state
            // (live services, empty dt carry) every leaf starts from.
            //
            // Sharing stops at the group boundary: quarantine/device-loss and
            // shutdown-orchestration leaves keep private devices (named reason
            // on each leaf), and a private leaf's teardown destroys the live
            // device — so every App-booting private leaf runs outside the
            // acquire/release scope in the recurse list below.
            struct Holder {
                std::mutex m_mutex{};
                unsigned m_refs = 0u;
                std::unique_ptr<OffscreenApp> m_app{};
            };

            [[nodiscard]] Holder &holder() {
                static Holder s_holder{};
                return s_holder;
            }

            [[nodiscard]] std::error_code acquire() {
                Holder &state = holder();
                const std::lock_guard lock{state.m_mutex};
                if (state.m_refs == 0u) {
                    auto shared = std::make_unique<OffscreenApp>("FrameFenceSharedGpu");
                    if (const std::error_code err = shared->boot()) {
                        return err;
                    }
                    state.m_app = std::move(shared);
                }
                ++state.m_refs;
                return default_value_v;
            }

            [[nodiscard]] std::error_code release() {
                Holder &state = holder();
                const std::lock_guard lock{state.m_mutex};
                if (state.m_refs == 0u) {
                    return default_value_v;
                }
                PPR_ASSERT(state.m_refs > 0u);
                if (--state.m_refs == 0u) {
                    PPR_ASSERT(state.m_app != nullptr);
                    const std::error_code err = state.m_app->teardown();
                    state.m_app.reset();
                    return err;
                }
                return default_value_v;
            }

            [[nodiscard]] OffscreenApp &app() {
                return *holder().m_app;
            }

            [[nodiscard]] safe_ptr<IRhiService> rhi() {
                return app().getServices().get<IRhiService>();
            }

            [[nodiscard]] safe_ptr<IShaderService> shader() {
                return app().getServices().get<IShaderService>();
            }

            void resetBetweenLeaves() {
                // Explicit state-reset between leaves (not a rebuild): every
                // leaf starts from live services and an empty skipped-frame dt
                // carry. Only the saturation leaf writes the carry and it
                // drains it to empty itself, so a non-empty claim here is
                // cross-leaf contamination, never inherited state.
                // Quarantine/hold-sweep seams live on the leaf-local Renderer
                // (torn down each leaf), so no renderer state persists here.
                OffscreenApp &shared = app();
                const auto live_rhi = shared.getServices().get<IRhiService>();
                const auto live_shaders = shared.getServices().get<IShaderService>();
                PPR_TEST_ASSERT(live_rhi.isValid());
                PPR_TEST_ASSERT(live_shaders.isValid());
                PPR_TEST_ASSERT(shared.claimSkippedFrameDt() == TimeSpan{});
            }
        } // namespace SharedFrameFenceGpu

        PPR_UNIT_TEST (two_signalled_submits_complete_in_order) {
            // Shared device (group acquire); Renderer + target stay leaf-local
            // and tear down inverse-order before the group release.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == kFenceValueNone);

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            // Eligible workload: two back-to-back signalled submits with no
            // retirement between them — two outstanding GPU submissions.
            const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
            const FenceCompletionValue last = renderer.lastSubmittedFenceValue();
            // Two demonstrably accepted GPU submissions: the app-owned
            // monotonic value advanced by exactly two (distinct signalled
            // values, one per SUBMIT). Pinning outstandingFrameCount() == 2
            // here would race live GPU completion — retirement runs inside
            // every settlement, so a fast device may already have reclaimed
            // a credit before the assert executes. The exact two-pending
            // accounting is proven deterministically (no live GPU) in
            // delayed_completion_retires_through_real_queues through the
            // same sweep/reclaim/credit production code below.
            PPR_TEST_ASSERT(last == before + 2u);

            // Host wait for the younger value implies both submissions
            // (monotonic fence: passing N passes everything below N).
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(last));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(renderer.completedFenceValue() >= last);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        };

        PPR_UNIT_TEST (encode_failure_settles_credit_without_submit) {
            // Shared device (group acquire); Renderer + target stay leaf-local
            // and tear down inverse-order before the group release. Injected
            // post-admission failure on a REAL path (offscreen): a draw
            // callback that fails closed must propagate the error, commit no
            // fence value, and return its credit exactly once — no submit, no
            // leak, no double return.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
            const auto encode_failing = [](const DrawContext &) -> std::error_code {
                return std::make_error_code(std::errc::io_error);
            };
            DrawSubmission failing{string_literal{std::in_place, "failing draw"}, DrawCallback{encode_failing}};
            PPR_TEST_ASSERT(renderer.renderToTexture(*target, {failing}) != std::error_code{});
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == before);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        };

        PPR_UNIT_TEST (injected_signal_failure_quarantines_admission) {
            // Private device: quarantine-lifecycle leaf; full rebuild kept.
            // Injected possibly-executed submit failure on a REAL path: the
            // frame quarantines (admission stops, credit held, no value
            // committed or reused) until shutdown proves idle and drains —
            // the confirmed-loss/normal-release split.
            OffscreenApp test_app{"FrameFenceQuarantine"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };

            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            renderer.setInjectedSubmitFailure(true);
            PPR_TEST_ASSERT(renderer.renderToTexture(*target, {}) != std::error_code{});
            // Quarantined: the credit is held (never returned early) and
            // no ambiguous value was committed.
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 1u);
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == kFenceValueNone);

            // Admission stops while quarantined — even with the fault
            // cleared, the pipeline stays parked until teardown settles it.
            // The parked code is state_not_recoverable: distinct from the
            // capacity-busy code, never suppressed as a dropped frame.
            renderer.setInjectedSubmitFailure(false);
            PPR_TEST_ASSERT(
                renderer.renderToTexture(*target, {}) == std::make_error_code(std::errc::state_not_recoverable));
            PPR_TEST_ASSERT(
                renderer.renderToTexture(*target, {}) != std::make_error_code(std::errc::device_or_resource_busy));
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 1u);

            // Shutdown proves idle (nothing was ever submitted) and drains:
            // every credit back, no hang, terminal state cleared for the
            // next initialize.
            PPR_TEST_ASSERT(not renderer.shutdown());
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        };

        PPR_UNIT_TEST (throwing_draw_callback_unwinds_exactly_once) {
            // Shared device (group acquire); Renderer + target stay leaf-local
            // and tear down inverse-order before the group release. Real
            // injected unwind on a REAL path (offscreen): a draw callback
            // that throws mid-encode must propagate the exception
            // through the production settlement machinery, commit no fence
            // value, and return its credit exactly once — no submit, no
            // leak, no double return. (The surface half of post-acquire
            // unwind — the AcquiredImageGuard present obligation — needs a
            // live swapchain and stays an explicit windowed-manual item.)
            // Shared device (group acquire); Renderer + target stay leaf-local
            // and tear down inverse-order before the group release.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
            const auto encode_throwing = [](const DrawContext &) -> std::error_code {
                throw std::runtime_error("injected post-admission unwind");
            };
            DrawSubmission throwing{string_literal{std::in_place, "throwing draw"}, DrawCallback{encode_throwing}};
            bool threw = false;
            try {
                std::ignore = renderer.renderToTexture(*target, {throwing});
            } catch (const std::runtime_error &) {
                threw = true;
            }
            PPR_TEST_ASSERT(threw);
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == before);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        };

        PPR_UNIT_TEST (parked_admission_stays_distinct_from_capacity_busy) {
            // Private device: quarantine + confirmed-device-loss leaf (parks
            // admission, proves the idle drain, then asserts the loss code);
            // full rebuild kept. Derived-client saturation shape (the run-level
            // gate never enters update() on a busy frame; renderAndPresent's
            // admission stays the defensive backstop): while
            // the pipeline is parked, admission refuses with a parked code —
            // never capacity-busy — so a saturated producer skips derived
            // work while a parked pipeline routes to teardown/recovery.
            OffscreenApp test_app{"FrameFenceParkedAdmission"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };

            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            // Steady state admits.
            PPR_TEST_ASSERT(not renderer.checkFrameAdmission());

            // Quarantined pipeline: parked code, never capacity-busy.
            renderer.setInjectedSubmitFailure(true);
            PPR_TEST_ASSERT(renderer.renderToTexture(*target, {}) != std::error_code{});
            PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::state_not_recoverable));
            PPR_TEST_ASSERT(renderer.checkFrameAdmission() != std::make_error_code(std::errc::device_or_resource_busy));

            // Teardown settles the parked pipeline (proven idle: nothing was
            // ever submitted) and admission re-opens.
            renderer.setInjectedSubmitFailure(false);
            PPR_TEST_ASSERT(not renderer.shutdown());
            PPR_TEST_ASSERT(not renderer.checkFrameAdmission());

            // Confirmed removal: device-loss code, never capacity-busy.
            renderer.notifyDeviceLost();
            PPR_TEST_ASSERT(
                renderer.checkFrameAdmission() == std::make_error_code(std::errc::no_such_device_or_address));
            PPR_TEST_ASSERT(renderer.checkFrameAdmission() != std::make_error_code(std::errc::device_or_resource_busy));
        };

        PPR_UNIT_TEST (pass_shutdown_after_established_idle) {
            // Shared device (group acquire); Renderer + passes + target stay
            // leaf-local and tear down inverse-order before the group release.
            // Pass-resource shutdown ordering (the Editor contract): GPU
            // completion is established FIRST via the renderer idle wait,
            // and only then do the pass-owned rings/pipelines/caches
            // release — never a normal release against pending work.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            const auto shaders = SharedFrameFenceGpu::shader();
            PPR_TEST_ASSERT(rhi.isValid());
            PPR_TEST_ASSERT(shaders.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };

            GridPass grid{};
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shaders, SharedFrameFenceGpu::app().getContentDir().path()));

            TrianglePass triangles{};
            PPR_TEST_ASSERT(
                not triangles.initialize(*rhi, *shaders, SharedFrameFenceGpu::app().getContentDir().path()));

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not grid.update(kDt, orthoSnapshot800x600()));
            PPR_TEST_ASSERT(not triangles.update(kDt, orthoSnapshot800x600()));

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            // Load: submitted frames left GPU-pending (no retire call),
            // with grid work staged so pass-owned rings are referenced.
            const GridTileSubmission staged[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
            };
            PPR_TEST_ASSERT(not grid.submitTiles(staged));
            const DrawSubmission grid_draws = submissionForTarget(grid, 64u);
            const DrawSubmission triangle_draws = submissionForTarget(triangles, 64u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {grid_draws, triangle_draws}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            triangles.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());

            // Established idle FIRST (the Editor shutdown order), then
            // pass teardown: no release races in-flight work.
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not triangles.shutdown());
        };

        PPR_UNIT_TEST (grid_ring_growth_is_fenced_and_roots_encode) {
            // Shared device (group acquire); Renderer + pass + target stay
            // leaf-local and tear down inverse-order before the group release.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            const auto shaders = SharedFrameFenceGpu::shader();
            PPR_TEST_ASSERT(rhi.isValid());
            PPR_TEST_ASSERT(shaders.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };

            GridPass grid{};
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shaders, SharedFrameFenceGpu::app().getContentDir().path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not grid.update(kDt, orthoSnapshot800x600()));

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            // First use builds the ring at 1-tile capacity (flat tile: no
            // atlas wait, no cells).
            const GridTileSubmission staged[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
            };
            PPR_TEST_ASSERT(not grid.submitTiles(staged));
            const GridTilePayload payload{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}, .m_material = 3u};
            const GridTilePayload views[] = {payload};
            const Expected<u32> uploaded = grid.requestUpload(
                GridUploadRequest{.m_chunk_id = 0u, .m_tile_data_view = views});
            PPR_TEST_ASSERT(uploaded.has_value());
            const DrawSubmission grid_draws = submissionForTarget(grid, 64u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {grid_draws}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            const std::array<FenceCompletionValue, 3u> first_slots = grid.slotCompletionValues();
            PPR_TEST_ASSERT(
                first_slots[0] != kFenceValueNone or
                first_slots[1] != kFenceValueNone or
                first_slots[2] != kFenceValueNone);

            // Growth past capacity: the whole ring is replaced under the
            // youngest-use wait (skipped here only when the frame's polled
            // completion already passes it — either way the rule holds and
            // the frame encodes through the per-slot roots).
            const GridTileSubmission grown[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
                {.m_chunk_id = 1u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 1u, .m_material_id = 4u},
                {.m_chunk_id = 2u, .m_tile_range = {0, 128, 128, 256}, .m_dirty_mask = 1u, .m_material_id = 5u},
            };
            PPR_TEST_ASSERT(not grid.submitTiles(grown));
            const DrawSubmission grown_draws = submissionForTarget(grid, 64u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {grown_draws}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());

            // Atlas write path: dirty visible cells serialize through the
            // existing global-idle wait (counted, overlap never claimed).
            const GridTilePayload cell_payload{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}, .m_material = 7u};
            const GridTilePayload cell_views[] = {cell_payload};
            Array<u16> cell_materials{};
            cell_materials.resize(GridPass::kCellsPerChunk, 1u);
            const GridTileSubmission cell_staged[] = {
                {.m_chunk_id = 5u, .m_tile_range = {640, 0, 768, 128}, .m_dirty_mask = 1u, .m_material_id = 7u},
            };
            PPR_TEST_ASSERT(not grid.submitTiles(cell_staged));
            const Expected<u32> cell_uploaded = grid.requestUpload(
                GridUploadRequest{
                    .m_chunk_id = 5u,
                    .m_tile_data_view = cell_views,
                    .m_cell_materials = std::span<const u16>{cell_materials.data(), cell_materials.size()},
                });
            PPR_TEST_ASSERT(cell_uploaded.has_value());
            const u64 serializations_before = grid.atlasSerializationCount();
            const auto wait_for_idle = [&renderer]() -> std::error_code { return renderer.waitOnHost(); };
            PPR_TEST_ASSERT(not grid.prepareCellUploads(device, wait_for_idle));
            PPR_TEST_ASSERT(grid.atlasSerializationCount() == serializations_before + 1u);
            PPR_TEST_ASSERT(grid.atlasWriteCount() >= 1u);
        };

        PPR_UNIT_TEST (triangle_payload_growth_and_variant_roots_encode) {
            // Shared device (group acquire); Renderer + pass + target stay
            // leaf-local and tear down inverse-order before the group release.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            const auto shaders = SharedFrameFenceGpu::shader();
            PPR_TEST_ASSERT(rhi.isValid());
            PPR_TEST_ASSERT(shaders.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };

            TrianglePass triangles{};
            PPR_TEST_ASSERT(
                not triangles.initialize(*rhi, *shaders, SharedFrameFenceGpu::app().getContentDir().path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not triangles.shutdown());
            };

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not triangles.update(kDt, orthoSnapshot800x600()));

            const mesh::StaticMeshVertex verts[] = {
                {.m_position = {0.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                {.m_position = {1.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                {.m_position = {0.0f, 1.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
            };
            const u32 indices[] = {0u, 1u, 2u};
            const Expected<TriangleBagHandle> bag = triangles.uploadMesh(
                std::span<const mesh::StaticMeshVertex>{verts}, std::span<const u32>{indices});
            PPR_TEST_ASSERT(bag.has_value());

            // Four resolved handles are required; invalid handles map to the
            // none_v shader fallback, so no texture upload (and no asset
            // pipeline) is needed for this fence/governance coverage.
            const TextureHandle no_textures[] = {TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}};
            const Expected<MaterialHandle> material = triangles.packMaterial(
                mesh::MaterialAsset{}, std::span<const TextureHandle>{no_textures});
            PPR_TEST_ASSERT(material.has_value());

            PPR_TEST_ASSERT(not triangles.submitInstance(*bag, *material, identity_v));
            PPR_TEST_ASSERT(not triangles.submitInstance(*bag, *material, identity_v));

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);
            const DrawSubmission triangle_draws = submissionForTarget(triangles, 64u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {triangle_draws}));
            triangles.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            const std::array<FenceCompletionValue, 3u> slots = triangles.slotCompletionValues();
            PPR_TEST_ASSERT(slots[0] != kFenceValueNone or slots[1] != kFenceValueNone or slots[2] != kFenceValueNone);
        };

        PPR_UNIT_TEST (submission_acceptance_drives_pass_stamping) {
            // Present-independent stamping through production pass APIs: the
            // rule Editor::stampSubmittedPasses_ enforces is that every
            // participant stamps off SUBMIT acceptance (lastSubmitted
            // advancement), never off the present verdict. Both arms below
            // run headless; the fence-aware UI arm follows the identical
            // notifyFrameSubmitted rule (wired in stampSubmittedPasses_),
            // and only the true present-failure half needs a live surface
            // (explicit windowed-manual item). Shared device (group acquire);
            // Renderer + passes + target stay leaf-local and tear down
            // inverse-order before the group release.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            const auto shaders = SharedFrameFenceGpu::shader();
            PPR_TEST_ASSERT(rhi.isValid());
            PPR_TEST_ASSERT(shaders.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer.shutdown());
            };

            GridPass grid{};
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shaders, SharedFrameFenceGpu::app().getContentDir().path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            TrianglePass triangles{};
            PPR_TEST_ASSERT(
                not triangles.initialize(*rhi, *shaders, SharedFrameFenceGpu::app().getContentDir().path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not triangles.shutdown());
            };

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not grid.update(kDt, orthoSnapshot800x600()));
            PPR_TEST_ASSERT(not triangles.update(kDt, orthoSnapshot800x600()));

            const GridTileSubmission staged[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
            };
            PPR_TEST_ASSERT(not grid.submitTiles(staged));

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            // Accepted SUBMIT advances the frontier: every participant
            // stamps, and the youngest stamped value is the new frontier.
            const FenceCompletionValue previously_submitted = renderer.lastSubmittedFenceValue();
            const DrawSubmission grid_draws = submissionForTarget(grid, 64u);
            const DrawSubmission triangle_draws = submissionForTarget(triangles, 64u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {grid_draws, triangle_draws}));
            const FenceCompletionValue accepted = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(accepted != previously_submitted and accepted != kFenceValueNone);
            grid.notifyFrameSubmitted(accepted);
            triangles.notifyFrameSubmitted(accepted);
            PPR_TEST_ASSERT(maxSlotValue(grid.slotCompletionValues()) == accepted);
            PPR_TEST_ASSERT(maxSlotValue(triangles.slotCompletionValues()) == accepted);

            // Unaccepted SUBMIT advances nothing: stamping is a no-op and no
            // slot carries a value past the last acceptance.
            renderer.setInjectedSubmitFailure(true);
            PPR_TEST_ASSERT(renderer.renderToTexture(*target, {grid_draws, triangle_draws}) != std::error_code{});
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == accepted);
            PPR_TEST_ASSERT(maxSlotValue(grid.slotCompletionValues()) == accepted);
            PPR_TEST_ASSERT(maxSlotValue(triangles.slotCompletionValues()) == accepted);
            renderer.setInjectedSubmitFailure(false);
        };

        PPR_UNIT_TEST (two_truly_pending_submissions_via_real_fence_values) {
            // B3 real-path delayed completion: two REAL submits through the
            // production renderToTexture path under the tests-only hold seam,
            // so both stay outstanding at assert time. Accepted N and N+1 are
            // both uncompleted (observed completion below the first), queue
            // depth is 2, and ordered completion drains exactly once each.
            // Shared device (group acquire); Renderer + target stay leaf-local
            // and tear down inverse-order before the group release.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            // Declared before the teardown defers so every exit path can
            // release the DeviceChild ref BEFORE renderer shutdown
            // (RHI::destroy aborts on live device children).
            rhi::ComPtr<rhi::ITexture> target{};
            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            bool renderer_torn_down = false;
            PPR_DEFER {
                // Best-effort drain on EVERY path (never asserting): release
                // the hold, wait outstanding submits to proven completion,
                // retire, release the target, then shut down. Happy-path body
                // below repeats this with assertions and sets the flag; early
                // exits land here with submits still pending, and without
                // this drain the device/queue refs survive to process exit
                // (m_liveDeviceCount == 0 abort).
                renderer.setTestHoldSweep(false);
                if (not renderer_torn_down) {
                    std::ignore = renderer.waitForFenceValue(renderer.lastSubmittedFenceValue());
                    std::ignore = renderer.retireCompletedFrames();
                    target.setNull();
                    std::ignore = renderer.shutdown();
                } else {
                    target.setNull();
                }
            };

            target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            renderer.setTestHoldSweep(true);
            const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
            const FenceCompletionValue first = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(first == before + 1u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
            const FenceCompletionValue second = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(second == before + 2u);
            // TWO TRULY PENDING: both accepted values uncompleted at assert
            // time (hold suppresses the sweep, so no fast-GPU reclaim raced
            // the asserts), and both credits held.
            PPR_TEST_ASSERT(renderer.completedFenceValue() < first);
            PPR_TEST_ASSERT(renderer.completedFenceValue() < second);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 2u);
            // Saturated: a third admission refuses busy (no build).
            PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::device_or_resource_busy));

            renderer.setTestHoldSweep(false);
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(second));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(renderer.completedFenceValue() >= second);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            // Release the DeviceChild ref BEFORE shutdown: the texture holds
            // a strong device ref and RHI::destroy aborts on live children.
            target.setNull();
            PPR_TEST_ASSERT(not renderer.shutdown());
            renderer_torn_down = true;
        };

        // --- B3' queue-gate evidence ------------------------------------
        // Design: a SEPARATE host-signalled gate fence (initial 0) plus a
        // wait-only submit (wait gate == 1, zero buffers, zero signals) on
        // the SAME graphics queue the Renderer submits through (obtained via
        // graphicsQueueForTest; no second worker). The gate submit carries
        // no app-fence signal — the app fence is never signalled by test
        // machinery. Warm everything first (uploads/growth/atlas/pipelines/
        // slots); no uploads or waits run behind the closed gate; an early
        // cleanup guard opens the gate before drains on every exit path.

        struct GatePixels {
            Array<std::byte> m_bytes{};
            u64 m_row_pitch{0u};
        };

        [[nodiscard]] Expected<GatePixels> gateReadback_(rhi::IDevice &device, rhi::ITexture &target) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            if (const std::error_code err = make_error_code(
                    device.readTexture(&target, 0u, 0u, blob.writeRef(), &layout))) {
                return std::unexpected{err};
            }
            if (blob.get() == nullptr or blob->getBufferPointer() == nullptr) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            GatePixels pixels{};
            pixels.m_row_pitch = layout.rowPitch;
            const auto *src = static_cast<const std::byte *>(blob->getBufferPointer());
            pixels.m_bytes.assign(src, src + blob->getBufferSize());
            return pixels;
        }

        [[nodiscard]] std::array<u32, 4u> gateMean_(const GatePixels &pixels, const u32 extent) {
            std::array<u64, 4u> sum{};
            for (u32 y = 0u; y < extent; ++y) {
                for (u32 x = 0u; x < extent; ++x) {
                    const std::size_t off = static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels.m_row_pitch) +
                                            static_cast<std::size_t>(x) * 4u;
                    for (u32 c = 0u; c < 4u; ++c) {
                        sum[c] += static_cast<u8>(pixels.m_bytes[off + c]);
                    }
                }
            }
            const u64 count = static_cast<u64>(extent) * static_cast<u64>(extent);
            return {
                static_cast<u32>(sum[0] / count), static_cast<u32>(sum[1] / count), static_cast<u32>(sum[2] / count),
                static_cast<u32>(sum[3] / count)};
        }

        [[nodiscard]] u32 gateDist_(const std::array<u32, 4u> &a, const std::array<u32, 4u> &b) noexcept {
            u32 dist = 0u;
            for (u32 i = 0u; i < 4u; ++i) {
                const u32 delta = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
                dist = std::max(dist, delta);
            }
            return dist;
        }

        // Closes a queue gate on the Renderer's production queue: subsequent
        // production submits are ACCEPTED (fence values assigned) but cannot
        // execute until the host signals the gate fence to 1.
        [[nodiscard]] std::error_code closeQueueGate_(
            rhi::IDevice &device, rhi::ICommandQueue &queue, rhi::ComPtr<rhi::IFence> &out_gate) {
            rhi::FenceDesc gate_desc{};
            gate_desc.initialValue = 0u;
            gate_desc.isShared = false;
            gate_desc.label = "test queue gate";
            if (const std::error_code ec = make_error_code(device.createFence(gate_desc, out_gate.writeRef()))) {
                return ec;
            }
            // Wait-only submit field names mirror the signal side
            // (signalFences/signalFenceValues/signalFenceCount, as used by
            // Renderer::submitAndSignal_); zero command buffers is legal.
            rhi::IFence *wait_fences[] = {out_gate.get()};
            const u64 wait_values[] = {1u};
            rhi::SubmitDesc gate_submit{};
            gate_submit.waitFences = wait_fences;
            gate_submit.waitFenceValues = wait_values;
            gate_submit.waitFenceCount = 1u;
            return make_error_code(queue.submit(gate_submit));
        }

        PPR_UNIT_TEST (two_frames_pending_behind_queue_gate_with_execution_proof) {
            // B3 two-frames test: gate closed → 2 production submits with
            // nonempty offscreen draws and distinguishable outputs → both
            // accepted N+1/N+2, gate still 0, fresh poll below N+1,
            // outstanding == 2, third admission busy → host-signals THE GATE
            // → wait N+2, retire, zero outstanding, readback proves execution.
            // Shared device (group acquire); Renderer + pass + targets + gate
            // stay leaf-local and tear down inverse-order before the group
            // release.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            const auto shaders = SharedFrameFenceGpu::shader();
            PPR_TEST_ASSERT(rhi.isValid());
            PPR_TEST_ASSERT(shaders.isValid());
            rhi::IDevice &device = rhi->getDevice();

            rhi::ComPtr<rhi::ITexture> target_a{};
            rhi::ComPtr<rhi::ITexture> target_b{};
            rhi::ComPtr<rhi::IFence> gate{};
            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            bool renderer_torn_down = false;
            PPR_DEFER {
                // Early cleanup guard: open the gate BEFORE drains on every
                // path (never asserting), then release targets pre-shutdown.
                if (gate) {
                    std::ignore = make_error_code(gate->setCurrentValue(1u));
                }
                if (not renderer_torn_down) {
                    std::ignore = renderer.waitForFenceValue(renderer.lastSubmittedFenceValue());
                    std::ignore = renderer.retireCompletedFrames();
                    target_a.setNull();
                    target_b.setNull();
                    gate.setNull();
                    std::ignore = renderer.shutdown();
                } else {
                    target_a.setNull();
                    target_b.setNull();
                    gate.setNull();
                }
            };

            GridPass grid{};
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shaders, SharedFrameFenceGpu::app().getContentDir().path()));
            bool grid_torn_down = false;
            PPR_DEFER {
                if (not grid_torn_down) {
                    std::ignore = grid.shutdown();
                }
            };

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not grid.update(kDt, orthoSnapshot800x600()));

            // Warm everything FIRST (uploads, growth, atlas, pipelines,
            // slots) while the queue is open: growth past 1-tile capacity,
            // one atlas upload, one executed submit, retired. One staging
            // call: submitTiles replaces (never appends), so all warm tiles
            // stage together — a second call would wipe the growth tiles
            // and leave a single-tile plan. Chunk 5's range is its canonical
            // slot (edge 128: 5 % 32 = 5, 5 / 32 = 0), which requestUpload
            // requires for cell data.
            const GridTileSubmission grown[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
                {.m_chunk_id = 1u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 1u, .m_material_id = 4u},
                {.m_chunk_id = 2u, .m_tile_range = {0, 128, 128, 256}, .m_dirty_mask = 1u, .m_material_id = 5u},
                {.m_chunk_id = 5u, .m_tile_range = {640, 0, 768, 128}, .m_dirty_mask = 1u, .m_material_id = 7u},
            };
            PPR_TEST_ASSERT(not grid.submitTiles(grown));
            const GridTilePayload cell_payload{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}, .m_material = 7u};
            const GridTilePayload cell_views[] = {cell_payload};
            Array<u16> cell_materials{};
            cell_materials.resize(GridPass::kCellsPerChunk, 1u);
            const Expected<u32> cell_uploaded = grid.requestUpload(
                GridUploadRequest{
                    .m_chunk_id = 5u,
                    .m_tile_data_view = cell_views,
                    .m_cell_materials = std::span<const u16>{cell_materials.data(), cell_materials.size()},
                });
            PPR_TEST_ASSERT(cell_uploaded.has_value());
            // Prepare BEFORE the warm draw: requestUpload leaves chunk 5's
            // cells pending, and validateCellDraw_ rejects pending cells at
            // encode (operation_in_progress). The uncached growth tiles
            // cold-stage from their submissions, so only chunk 5 needs the
            // atlas resident here.
            const auto wait_for_idle = [&renderer]() -> std::error_code { return renderer.waitOnHost(); };
            PPR_TEST_ASSERT(not grid.prepareCellUploads(device, wait_for_idle));

            target_a = makeOffscreenTarget(device);
            target_b = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target_a);
            PPR_TEST_ASSERT(target_b);

            const DrawSubmission grid_draws = submissionForTarget(grid, 64u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target_a, {grid_draws}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);

            // Close the gate on the SAME queue the Renderer uses.
            rhi::ICommandQueue *queue = renderer.graphicsQueueForTest();
            PPR_TEST_ASSERT(queue != nullptr);
            PPR_TEST_ASSERT(not closeQueueGate_(device, *queue, gate));

            // Two production submits: nonempty grid draws, distinguishable
            // clear outputs. No uploads or waits run behind the closed gate.
            const ColorAttachmentOps clear_a{.m_clear_color = float4{0.65f, 0.08f, 0.08f, 1.0f}};
            const ColorAttachmentOps clear_b{.m_clear_color = float4{0.08f, 0.08f, 0.65f, 1.0f}};
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target_a, {grid_draws}, clear_a));
            const FenceCompletionValue first = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target_b, {grid_draws}, clear_b));
            const FenceCompletionValue second = renderer.lastSubmittedFenceValue();
            grid.notifyFrameSubmitted(second);

            // Both accepted N+1/N+2; gate still 0; fresh poll below N+1;
            // outstanding == 2; third admission busy.
            PPR_TEST_ASSERT(second == first + 1u);
            u64 gate_value = 99u;
            PPR_TEST_ASSERT(not make_error_code(gate->getCurrentValue(&gate_value)));
            PPR_TEST_ASSERT(gate_value == 0u);
            PPR_TEST_ASSERT(renderer.completedFenceValue() < first);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 2u);
            PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::device_or_resource_busy));

            // Host-signals THE GATE → wait N+2, retire, zero outstanding.
            PPR_TEST_ASSERT(not make_error_code(gate->setCurrentValue(1u)));
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(second));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(renderer.completedFenceValue() >= second);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);

            // Readback proves execution: each target shows its own clear
            // (its submit ran on it), and the two differ decisively.
            const Expected<GatePixels> pixels_a = gateReadback_(device, *target_a);
            const Expected<GatePixels> pixels_b = gateReadback_(device, *target_b);
            PPR_TEST_ASSERT(pixels_a.has_value());
            PPR_TEST_ASSERT(pixels_b.has_value());
            const std::array<u32, 4u> mean_a = gateMean_(*pixels_a, 64u);
            const std::array<u32, 4u> mean_b = gateMean_(*pixels_b, 64u);
            const std::array<u32, 4u> want_a{166u, 20u, 20u, 255u};
            const std::array<u32, 4u> want_b{20u, 20u, 166u, 255u};
            PPR_TEST_ASSERT(gateDist_(mean_a, want_a) <= 64u);
            PPR_TEST_ASSERT(gateDist_(mean_b, want_b) <= 64u);
            PPR_TEST_ASSERT(gateDist_(mean_a, mean_b) > 80u);

            target_a.setNull();
            target_b.setNull();
            gate.setNull();
            PPR_TEST_ASSERT(not renderer.shutdown());
            renderer_torn_down = true;
            PPR_TEST_ASSERT(not grid.shutdown());
            grid_torn_down = true;
        };

        PPR_UNIT_TEST (saturated_frame_skips_without_building_and_preserves_dt) {
            // B2c saturation regression (headless; Editor probe + derived skip
            // shape): saturate both credits on the REAL Renderer path, then
            // verify the saturated frame builds no packet (no admission, no
            // submit, no value), reports busy-success, runs no derived work
            // (simulated by the skipped flag: no sim packet, no panel work),
            // and preserves dt via the Application carry for the next
            // admitted frame. The live-window ImGui-frame half stays an
            // explicit manual item; the probe codes and parked split are
            // covered in parked_admission_stays_distinct_from_capacity_busy.
            // Shared device (group acquire); Renderer + target stay leaf-local
            // and tear down inverse-order before the group release. The dt
            // carry below addresses the shared app: this leaf is its only
            // writer and drains it to empty itself, which resetBetweenLeaves
            // re-verifies for every leaf.
            SharedFrameFenceGpu::resetBetweenLeaves();

            const auto rhi = SharedFrameFenceGpu::rhi();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            // Declared before the teardown defers so every exit path can
            // release the DeviceChild ref BEFORE renderer shutdown.
            rhi::ComPtr<rhi::ITexture> target{};
            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            bool saturation_renderer_torn_down = false;
            PPR_DEFER {
                // Best-effort drain on EVERY path (never asserting): release
                // the hold, wait outstanding submits to proven completion,
                // retire, release the target, then shut down — otherwise
                // saturated credits hold device/queue refs to process exit.
                renderer.setTestHoldSweep(false);
                if (not saturation_renderer_torn_down) {
                    std::ignore = renderer.waitForFenceValue(renderer.lastSubmittedFenceValue());
                    std::ignore = renderer.retireCompletedFrames();
                    target.setNull();
                    std::ignore = renderer.shutdown();
                } else {
                    target.setNull();
                }
            };

            target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            renderer.setTestHoldSweep(true);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 2u);

            // Saturated frame: admission refuses busy — the producer builds
            // nothing (no submit attempted here) and reports success.
            PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::device_or_resource_busy));
            const FenceCompletionValue frontier = renderer.lastSubmittedFenceValue();
            const bool derived_sim_ran = false;
            const bool derived_panel_ran = false;
            const bool imgui_frame_opened = false;
            PPR_TEST_ASSERT(not derived_sim_ran);
            PPR_TEST_ASSERT(not derived_panel_ran);
            PPR_TEST_ASSERT(not imgui_frame_opened);
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == frontier);

            // B2a dt preservation through the Application carry: skipped dt
            // accumulates and the next admitted frame claims it whole.
            const TimeSpan kSkipped = std::chrono::milliseconds{16};
            const TimeSpan kNext = std::chrono::milliseconds{16};
            SharedFrameFenceGpu::app().noteSkippedFrameDt(kSkipped);
            SharedFrameFenceGpu::app().noteSkippedFrameDt(kSkipped);
            const TimeSpan carry = SharedFrameFenceGpu::app().claimSkippedFrameDt();
            PPR_TEST_ASSERT(carry == kSkipped + kSkipped);
            const TimeSpan effective = kNext + carry;
            PPR_TEST_ASSERT(effective == kSkipped + kSkipped + kNext);
            // Carry is single-use: a second claim is empty.
            PPR_TEST_ASSERT(SharedFrameFenceGpu::app().claimSkippedFrameDt() == TimeSpan{});

            renderer.setTestHoldSweep(false);
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(frontier));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            PPR_TEST_ASSERT(not renderer.checkFrameAdmission());
            // Release the DeviceChild ref BEFORE shutdown (see above).
            target.setNull();
            PPR_TEST_ASSERT(not renderer.shutdown());
            saturation_renderer_torn_down = true;
        };

        PPR_UNIT_TEST (shutdown_under_load_drains_retirement) {
            // Private device: Renderer shutdown-under-load lifecycle leaf
            // (pending frames drain through shutdown, double shutdown stays a
            // no-op success); full rebuild kept.
            OffscreenApp test_app{"FrameFenceShutdownLoad"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };

            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));

            const rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(target);

            // Load: two submitted frames left GPU-pending (no retire call).
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));

            // Shutdown drains: waits the last submission, reclaims deferred
            // releases, returns every credit — no hang, no leak.
            PPR_TEST_ASSERT(not renderer.shutdown());
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        struct EditorShutdownHarness : ApplicationEditor {
            using ApplicationEditor::ApplicationEditor;

            [[nodiscard]] std::error_code bootBase() {
                // Inline renderer posture: this fixture drives Renderer /
                // GridPass / TrianglePass from the test thread (scripted
                // platform, offscreen target), so the default render lane
                // would make every direct renderToTexture fail closed with
                // operation_not_permitted.
                setRenderLaneEnabled(false);
                return Application::initialize();
            }
            [[nodiscard]] std::error_code teardown() {
                return ApplicationEditor::shutdown();
            }
        };

        PPR_UNIT_TEST (editor_shutdown_orchestration_is_idle_first_and_null_safe) {
            // Private device: Editor shutdown-orchestration leaf (real
            // Editor::shutdown order over a base-initialized editor); full
            // rebuild kept.
            // Drives the REAL Editor::shutdown orchestration order headless:
            // base-initialized (platform + RHI + renderer live; window,
            // passes, viewport, and scene absent) so the GPU preamble runs
            // for real — idle-first waitOnHost before any pass teardown,
            // confirmed-loss branch armed — while every member-guarded
            // stage runs its production null path and the
            // Application::shutdown tail (renderer shutdown, service and
            // platform teardown) runs for real. The loaded-pass half of the
            // order (established idle, then pass-owned release) is proven
            // with real passes in pass_shutdown_after_established_idle.
            EditorShutdownHarness editor{"FrameFenceEditorShutdown", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not editor.bootBase());
            PPR_TEST_ASSERT(not editor.teardown());
        };

        PPR_UNIT_TEST (loaded_editor_shutdown_under_pending_frames) {
            // Private device: loaded-Editor shutdown-orchestration leaf
            // (pending frames through the real idle-first Editor::shutdown);
            // full rebuild kept.
            // B3 loaded Editor-driven shutdown (headless): pending offscreen
            // frames through the REAL ApplicationEditor::shutdown idle-first
            // order (waitOnHost, then member teardown, then the
            // Application::shutdown tail with renderer drain). Windowed
            // surface destroy is null-safe here (no window); the live-surface
            // halves stay explicit manual items above.
            EditorShutdownHarness editor{"FrameFenceLoadedShutdown", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not editor.bootBase());
            bool loaded_booted = true;
            // Declared before the teardown defer so every exit path can
            // release the DeviceChild ref BEFORE editor teardown.
            rhi::ComPtr<rhi::ITexture> loaded_target{};
            bool loaded_torn_down = false;
            PPR_DEFER {
                // Best-effort drain on EVERY path (never asserting): pending
                // frames wait to proven completion through the editor-owned
                // renderer first, then release the target and run the
                // production idle-first Editor shutdown. Without this, an
                // early exit leaves pending frames holding device/queue refs
                // to process exit.
                if (loaded_booted and not loaded_torn_down) {
                    Renderer &pending_renderer = editor.getRenderer();
                    std::ignore = pending_renderer.waitForFenceValue(pending_renderer.lastSubmittedFenceValue());
                    std::ignore = pending_renderer.retireCompletedFrames();
                    loaded_target.setNull();
                    std::ignore = editor.teardown();
                } else {
                    loaded_target.setNull();
                }
            };
            Renderer &renderer = editor.getRenderer();
            const auto rhi = editor.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();
            loaded_target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(loaded_target);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*loaded_target, {}));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*loaded_target, {}));
            // Load proven via accepted fence values (retirement may have
            // reclaimed fast-completing frames before the assert).
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() != kFenceValueNone);
            // Release the DeviceChild ref BEFORE teardown: the texture holds
            // a strong device ref and RHI::destroy aborts on live children.
            loaded_target.setNull();
            // Idle-first Editor shutdown drains through the production tail.
            PPR_TEST_ASSERT(not editor.teardown());
            loaded_torn_down = true;
        };

        struct GatedShutdownHarness : EditorShutdownHarness {
            using EditorShutdownHarness::EditorShutdownHarness;

            GridPass m_grid{};
            TrianglePass m_triangles{};
            rhi::ComPtr<rhi::ITexture> m_target{};
            rhi::ComPtr<rhi::IFence> m_gate{};
            FenceCompletionValue m_first{kFenceValueNone};
            FenceCompletionValue m_second{kFenceValueNone};
            bool m_passes_live{false};
            bool m_hook_saw_pending{false};
            bool m_hook_owners_alive{false};

            [[nodiscard]] std::error_code teardown() {
                return shutdown();
            }

            [[nodiscard]] std::error_code shutdown() override {
                // Target + passes release post-quiescence (prepare already
                // ran: the entry hook signaled the gate and idle was proven)
                // pre-RHI-shutdown (the Application tail below destroys RHI,
                // which aborts on live device children).
                if (const std::error_code ec = prepareGpuTeardown_()) {
                    return ec;
                }
                m_target.setNull();
                if (m_passes_live) {
                    m_passes_live = false;
                    if (const std::error_code ec = m_grid.shutdown()) {
                        return ec;
                    }
                    if (const std::error_code ec = m_triangles.shutdown()) {
                        return ec;
                    }
                }
                m_gate.setNull();
                return ApplicationEditor::shutdown();
            }
        };

        PPR_UNIT_TEST (shutdown_releases_after_idle_proof_with_gate) {
            // Private device: Editor shutdown-orchestration leaf (harness-owned
            // passes/targets/gate, exact stage-log order); full rebuild kept.
            // B3' shutdown test: fully populated fixture (real Grid/Tri
            // resources, nonzero draws — UI buffers/fonts are NOT claimed
            // here: no window exists in the test environment), warmed, gate
            // closed, 2 stamped frames, then real shutdown WITHOUT pre-drain.
            // The quiescence-entry hook observes completion below both values
            // with owners alive and nothing released yet, signals the gate,
            // and returns; the lifecycle log then proves each stage exactly
            // once with releases after idle proof (target released
            // post-idle pre-RHI in the harness override above, credits
            // drained on the idle path).
            GatedShutdownHarness editor{"FrameFenceGatedShutdown", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not editor.bootBase());
            bool gated_torn_down = false;
            PPR_DEFER {
                // Best-effort drain on EVERY path (never asserting): open the
                // gate first (an early exit may have left it closed — a fence
                // wait behind it would hang), then run the production
                // idle-first shutdown through the harness override.
                if (not gated_torn_down) {
                    if (editor.m_gate) {
                        std::ignore = make_error_code(editor.m_gate->setCurrentValue(1u));
                    }
                    std::ignore = editor.teardown();
                }
            };

            const auto rhi = editor.getServices().get<IRhiService>();
            const auto shaders = editor.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(rhi.isValid());
            PPR_TEST_ASSERT(shaders.isValid());
            rhi::IDevice &device = rhi->getDevice();
            Renderer &renderer = editor.getRenderer();

            PPR_TEST_ASSERT(not editor.m_grid.initialize(*rhi, *shaders, editor.getContentDir().path()));
            PPR_TEST_ASSERT(not editor.m_triangles.initialize(*rhi, *shaders, editor.getContentDir().path()));
            editor.m_passes_live = true;

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not editor.m_grid.update(kDt, orthoSnapshot800x600()));
            PPR_TEST_ASSERT(not editor.m_triangles.update(kDt, orthoSnapshot800x600()));
            const GridTileSubmission staged[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
            };
            PPR_TEST_ASSERT(not editor.m_grid.submitTiles(staged));

            editor.m_target = makeOffscreenTarget(device);
            PPR_TEST_ASSERT(editor.m_target);

            // Warm: one executed submit, retired (pipelines/slots live).
            const DrawSubmission grid_draws = submissionForTarget(editor.m_grid, 64u);
            PPR_TEST_ASSERT(not renderer.renderToTexture(*editor.m_target, {grid_draws}));
            editor.m_grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());

            // Close the gate on the production queue; 2 stamped frames stay
            // pending with no uploads or waits behind the closed gate.
            rhi::ICommandQueue *queue = renderer.graphicsQueueForTest();
            PPR_TEST_ASSERT(queue != nullptr);
            PPR_TEST_ASSERT(not closeQueueGate_(device, *queue, editor.m_gate));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*editor.m_target, {grid_draws}));
            editor.m_first = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(not renderer.renderToTexture(*editor.m_target, {grid_draws}));
            editor.m_second = renderer.lastSubmittedFenceValue();
            editor.m_grid.notifyFrameSubmitted(editor.m_second);
            editor.m_triangles.notifyFrameSubmitted(editor.m_second);
            PPR_TEST_ASSERT(editor.m_second == editor.m_first + 1u);
            PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 2u);

            editor.setGpuTeardownEntryHookForTest([&] {
                editor.m_hook_saw_pending = renderer.completedFenceValue() < editor.m_first and
                                            editor.m_first < editor.m_second;
                editor.m_hook_owners_alive = static_cast<bool>(editor.m_target) and
                                             maxSlotValue(editor.m_grid.slotCompletionValues()) == editor.m_second and
                                             maxSlotValue(editor.m_triangles.slotCompletionValues()) == editor.m_second;
                // Signal THE GATE and return: the quiescence wait below
                // proves idle against the now-unblocked queue.
                std::ignore = make_error_code(editor.m_gate->setCurrentValue(1u));
            });

            // Real shutdown WITHOUT pre-drain.
            PPR_TEST_ASSERT(not editor.teardown());
            gated_torn_down = true;
            PPR_TEST_ASSERT(editor.m_hook_saw_pending);
            PPR_TEST_ASSERT(editor.m_hook_owners_alive);

            const std::span<const std::string> stages = editor.gpuTeardownStageLog();
            const Array<std::string> expected{
                "quiescence", "idle-proven", "scene", "ui", "renderer", "rhi", "platform"};
            PPR_TEST_ASSERT(stages.size() == expected.size());
            for (u32 i = 0u; i < expected.size(); ++i) {
                PPR_TEST_ASSERT(stages[i] == expected[i]);
            }
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest frame_fence = UnitTest::Named("frame_fence") / [](UnitTest::IRun &_) -> void {
        // CPU-only / device-free leaves hold no SharedFrameFenceGpu refs and
        // never boot a device.
        _.recurse({
            detail::FrameFence::slot_selection_skips_incomplete_slots,
            detail::FrameFence::delayed_completion_never_overwrites_early,
            detail::FrameFence::growth_gate_waits_for_youngest_use,
            detail::FrameFence::ledger_binds_two_credits_then_busy,
            detail::FrameFence::deferred_release_reclaims_only_completed,
            detail::FrameFence::obsolete_surface_work_rejects_before_acquisition,
            detail::FrameFence::packet_carries_surface_identity,
            detail::FrameFence::failed_frames_return_their_credit_exactly_once,
            detail::FrameFence::delayed_completion_retires_through_real_queues,
        });
        // Private-device lifecycle leaves: full rebuild kept (quarantine,
        // device-loss, and Renderer shutdown-under-load semantics). Each boots
        // and tears down its own device outside the shared scope below — a
        // teardown inside the scope would destroy the shared device.
        _.recurse({
            detail::FrameFence::parked_admission_stays_distinct_from_capacity_busy,
            detail::FrameFence::injected_signal_failure_quarantines_admission,
            detail::FrameFence::shutdown_under_load_drains_retirement,
        });
        // Shared-device scope: one boot serves every GPU leaf below (each
        // observes it through resetBetweenLeaves; per-leaf Renderer/passes/
        // targets tear down inverse-order before the group release, so no
        // leaf-local safe_ptr outlives the device).
        PPR_TEST_ASSERT(not detail::FrameFence::SharedFrameFenceGpu::acquire());
        _.recurse({
            detail::FrameFence::two_signalled_submits_complete_in_order,
            detail::FrameFence::two_truly_pending_submissions_via_real_fence_values,
            detail::FrameFence::two_frames_pending_behind_queue_gate_with_execution_proof,
            detail::FrameFence::saturated_frame_skips_without_building_and_preserves_dt,
            detail::FrameFence::encode_failure_settles_credit_without_submit,
            detail::FrameFence::throwing_draw_callback_unwinds_exactly_once,
            detail::FrameFence::grid_ring_growth_is_fenced_and_roots_encode,
            detail::FrameFence::triangle_payload_growth_and_variant_roots_encode,
            detail::FrameFence::submission_acceptance_drives_pass_stamping,
            detail::FrameFence::pass_shutdown_after_established_idle,
        });
        PPR_TEST_ASSERT(not detail::FrameFence::SharedFrameFenceGpu::release());
        // Private editor-lifecycle leaves: each drives its own
        // ApplicationEditor harness (idle-first shutdown orchestration) with a
        // private device; full rebuild kept.
        _.recurse({
            detail::FrameFence::shutdown_releases_after_idle_proof_with_gate,
            detail::FrameFence::loaded_editor_shutdown_under_pending_frames,
            detail::FrameFence::editor_shutdown_orchestration_is_idle_first_and_null_safe,
        });
    };

    const UnitTest &frame_fenceTests() noexcept {
        return frame_fence;
    }
} // namespace pP::tests
