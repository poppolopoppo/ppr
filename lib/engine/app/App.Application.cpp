module;

#include "pP/Macros.h"

module engine.app;

import :application;
import :display;
import :display.settings;
import :frame.packet;
import :frame.render_lane;
import :platform;
import :renderer;
import :service.window;
import :window.monitor;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(App, debug, none)

    namespace {
        [[nodiscard]] std::error_code applicationExceptionError_() noexcept {
            try {
                throw;
            } catch (const std::bad_alloc &) {
                return std::make_error_code(std::errc::not_enough_memory);
            } catch (const std::system_error &error) {
                return error.code();
            } catch (...) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
        }

        template<typename Operation>
        std::error_code pumpOperationBestEffort_(Operation &&operation, std::error_code &first_error) noexcept {
            std::error_code error{};
            try {
                error = operation();
            } catch (...) {
                error = applicationExceptionError_();
            }
            if (error and error != std::make_error_code(std::errc::operation_canceled) and not first_error) {
                first_error = error;
                try {
                    PPR_LOG(
                        App, debug, "wait pump retains first error",
                        {{"category", error.category().name()}, {"value", error.value()}});
                } catch (...) {
                    // Only diagnostics are lost; remaining pump/wait phases still run with the retained cause.
                }
            }
            return error;
        }

    }

    Application::Application(
        ApplicationDomain domain, const std::string_view name, const std::span<const char *const> argv)
        : Application(std::move(domain), name, argv, IPlatform::create(), ITimerClock::steady(), nullptr) {
    }

    Application::Application(
        ApplicationDomain domain, const std::string_view name, const std::span<const char *const> argv,
        const GpuTeardownFailureHandler on_gpu_teardown_failure)
        : Application(
              std::move(domain), name, argv, IPlatform::create(), ITimerClock::steady(), on_gpu_teardown_failure) {
    }

    Application::Application(
        ApplicationDomain domain, const std::string_view name, const std::span<const char *const> argv,
        std::unique_ptr<IPlatform> platform, const ITimerClock &clock,
        const GpuTeardownFailureHandler on_gpu_teardown_failure)
        : m_application_clock(clock), m_pacer(clock), m_platform(std::move(platform)),
          m_arguments(argv.begin(), argv.end()), m_name(name), m_domain(std::move(domain)),
          m_on_gpu_teardown_failure(on_gpu_teardown_failure) {
        PPR_ASSERT(m_platform != nullptr);

        hal::disableSystemErrorReporting();
        hal::installDebugAssertHooks();

        hal::setThreadName("Application");
        // hal::setThreadPriority(hal::EPriority::high);
        this_thread::setGroup(thread::main_group);
    }

    Application::~Application() noexcept = default;

    std::optional<TimeSpan> Application::getTargetFrameTime() const noexcept {
        // Phase 3 overlay (§4.2): background priority activates the throttle
        // only when the legacy opt-out is off and display.throttle_unfocused
        // is enabled. The max() preserves a slower explicit cap (a paced 1fps
        // foreground stays 1fps) instead of speeding it up to 5fps; the
        // foreground mode (and vsync) is remembered and restored on refocus.
        const bool background = m_has_background_priority and not m_no_focus_throttle;
        return effectiveFramePeriod(m_target_frame_time, m_display_controller.throttleActive(background));
    }

    void Application::requestExit(const std::error_code clause) const noexcept {
        if (PPR_ENSURE(m_request_exit.isValid())) [[likely]] {
            if (not clause) {
                // Graceful exit is not cancellation: raise the exit latch so
                // the run loop terminates, but record no error — run() still
                // returns success.
                m_exit_requested.store(true, std::memory_order_release);
                // Wake any pacer/hint wait so the exit lands within one
                // slice instead of a full background period.
                requestFrameAdmissionWake();
            } else {
                m_request_exit(clause);
                requestFrameAdmissionWake();
            }
            try {
                PPR_LOG(
                    App, info, "request exit", {{"category", clause.category().name()}, {"cause", clause.message()}});
            } catch (...) {
                // Diagnostics only: losing the message cannot lose or alter the published exit cause.
            }
        }
    }

    void Application::requestFrameAdmissionWake() const noexcept {
        // Pure hint: coalesce the pulse, then wake the native wait (if
        // any). Both halves are any-thread safe; waiters recheck persistent
        // state and never admit on the hint alone.
        m_admission_wake.emitEvent();
        try {
            if (m_platform) {
                m_platform->postEmptyEvent();
            }
        } catch (...) {
            // Native wake is a hint; exit/error and the pulse remain durable, rechecked within one bounded slice.
        }
    }

    bool Application::isExitRequested() const noexcept {
        if (m_exit_requested.load(std::memory_order_acquire)) {
            return true;
        }
        return m_lifecycle and m_lifecycle->error();
    }

    void Application::setBackgroundPriority(const bool throttle) noexcept {
        m_has_background_priority = throttle;
    }

    void Application::setNoFocusThrottle(const bool enabled) noexcept {
        m_no_focus_throttle = enabled;
    }

    void Application::setTargetFrameDuration(const TimeDuration frame_time) noexcept {
        m_target_frame_time = std::chrono::duration_cast<TimeSpan>(frame_time);
    }

    void Application::setTargetFrameDuration(std::nullopt_t) noexcept {
        m_target_frame_time.reset();
    }

    void Application::setTargetFrameRate(const int fps) noexcept {
        if (fps > 0) {
            setTargetFrameDuration(TimeDuration{1.0 / static_cast<double>(fps)});
        } else {
            setTargetFrameDuration(std::nullopt);
        }
    }

    void Application::setDisplaySettings(const DisplaySettings &settings) {
        // Startup policy only; window intent is validated against native
        // capabilities during editor initialization. Runtime uses ack-only requests.
        m_display_controller.setCommitted(settings);
        const PacingTargets targets = pacingTargets(settings);
        m_target_frame_time = targets.m_target;
        m_presentation_vsync = targets.m_vsync;
        // Startup intent only. A constructed renderer has private presentation
        // state; runtime edits use requestDisplaySettings and commit on ack.
    }

    void Application::bindDisplaySurface_(
        const WindowHandle handle, const SurfaceGeneration observed_generation) noexcept {
        m_display_surface_handle = handle;
        m_windowed_geometry.reset();
        ++m_display_surface_lifetime;
        m_lane_surface_handle = handle;
        m_lane_surface_generation = observed_generation;
    }

    std::error_code Application::requestDisplaySettings(config::Resolved<DisplaySettings> candidate) {
        config::Diagnostics diagnostics{};
        return requestDisplaySettings(std::move(candidate), diagnostics);
    }

    Expected<DisplayCapabilities> Application::displayCapabilities() const {
        if (std::this_thread::get_id() != m_display_thread) {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        DisplayCapabilities capabilities{};
        const safe_ptr<IWindowService> service = m_platform->getWindowService();
        if (not service) {
            return capabilities;
        }
        capabilities.m_window_changes = service->supportsDisplayWindowChanges();
        capabilities.m_exclusive_refresh = service->supportsExclusiveRefresh();
        const auto modeView = [](const VideoMode &mode) {
            return DisplayModeCapability{
                mode.m_resolution.x > 0 ? static_cast<u32>(mode.m_resolution.x) : 0u,
                mode.m_resolution.y > 0 ? static_cast<u32>(mode.m_resolution.y) : 0u,
                mode.m_refresh_rate > 0 ? std::optional<u32>{static_cast<u32>(mode.m_refresh_rate)} : std::nullopt};
        };
        std::error_code modes_error{};
        const std::error_code error = service->enumerateMonitors([&](const SharedMonitor &monitor) {
            DisplayMonitorCapability entry{};
            entry.m_name = monitor->m_name;
            entry.m_primary = monitor->m_primary_monitor;
            entry.m_current = modeView(monitor->m_video_mode);
            const std::error_code enumerated = service->enumerateMonitorVideoModes(
                *monitor, [&](const VideoMode &mode) {
                    entry.m_modes.push_back(modeView(mode));
                    return std::error_code{};
                });
            if (enumerated and not modes_error) {
                modes_error = enumerated;
            }
            capabilities.m_monitors.push_back(std::move(entry));
            return std::error_code{};
        });
        if (error or modes_error) {
            return std::unexpected{error ? error : modes_error};
        }
        return capabilities;
    }

    Expected<WindowDisplayGeometry> Application::displayWindowGeometry() const {
        if (std::this_thread::get_id() != m_display_thread) {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        const safe_ptr<IWindowService> service = m_platform->getWindowService();
        if (not service or
            not service->getMainWindow() or
            service->getMainWindow()->m_handle != m_display_surface_handle) {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        return service->displayWindowGeometry(*service->getMainWindow());
    }

    std::error_code Application::requestDisplaySettings(
        config::Resolved<DisplaySettings> candidate, config::Diagnostics &diagnostics) {
        if (std::this_thread::get_id() != m_display_thread) {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (m_display_transactions_stopped or isExitRequested()) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        if (not m_display_surface_handle or not m_renderer) {
            return std::make_error_code(std::errc::not_connected);
        }
        const SurfaceGeneration generation = renderLaneOwnsRenderer_()
                                                 ? laneSurfaceGeneration(m_display_surface_handle)
                                                 : m_renderer->surfaceGenerationFor(m_display_surface_handle);
        if (generation == kSurfaceGenerationNone) {
            return std::make_error_code(std::errc::resource_unavailable_try_again);
        }
        const auto capabilities = displayCapabilities();
        if (not capabilities) {
            return capabilities.error();
        }
        const bool window_change = displayWindowChange(candidate.m_value, m_display_controller.committed());
        if (const std::error_code error = m_display_controller.begin(
                std::move(candidate), *capabilities, diagnostics)) {
            return error;
        }

        m_pending_surface_identity = {m_display_surface_handle, generation};
        m_pending_surface_lifetime = m_display_surface_lifetime;
        m_pending_window_change = window_change;
        m_window_operation_transaction = 0u;
        m_pending_window_geometry.reset();
        requestFrameAdmissionWake();
        return {};
    }

    Expected<WindowDisplayGeometry> Application::applyDisplayWindow_(
        const DisplaySettings &settings, const bool restore) {
        if (std::this_thread::get_id() != m_display_thread) {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        const safe_ptr<IWindowService> service = m_platform->getWindowService();
        if (not service or
            not service->getMainWindow() or
            service->getMainWindow()->m_handle != m_pending_surface_identity.m_handle) {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        const Window &window = *service->getMainWindow();
        if (not restore) {
            const auto geometry = service->displayWindowGeometry(window);
            if (not geometry) {
                return std::unexpected{geometry.error()};
            }
            if (geometry->m_size.x <= 0 or geometry->m_size.y <= 0) {
                return std::unexpected{std::make_error_code(std::errc::not_connected)};
            }
            m_restore_window_geometry = *geometry;
            m_restore_window_settings = m_display_controller.committed();
            m_restore_window_settings.m_width = static_cast<u32>(geometry->m_size.x);
            m_restore_window_settings.m_height = static_cast<u32>(geometry->m_size.y);
            m_restore_window_settings.m_window_mode = geometry->m_fullscreen ? EDisplayWindowMode::fullscreen
                                                                             : EDisplayWindowMode::windowed;
            m_restore_window_settings.m_monitor = geometry->m_fullscreen ? geometry->m_monitor : "auto";
            m_restore_window_settings.m_refresh_hz = geometry->m_fullscreen ? geometry->m_refresh_hz.value_or(0u) : 0u;
            if (not geometry->m_fullscreen) {
                m_windowed_geometry = *geometry;
            }
        }
        const auto capabilities = displayCapabilities();
        if (not capabilities) {
            return std::unexpected{capabilities.error()};
        }
        config::Diagnostics diagnostics{};
        if (const std::error_code error = validateDisplaySettings(settings, *capabilities, diagnostics)) {
            return std::unexpected{error};
        }

        SharedMonitor selected{};
        const std::error_code error = service->enumerateMonitors([&](const SharedMonitor &monitor) {
            if ((settings.m_monitor == "auto" and monitor->m_primary_monitor) or
                settings.m_monitor == monitor->m_name) {
                selected = monitor;
            }
            return std::error_code{};
        });
        if (error) {
            return std::unexpected{error};
        }
        if (settings.m_window_mode == EDisplayWindowMode::fullscreen and not selected) {
            return std::unexpected{std::make_error_code(std::errc::no_such_device)};
        }

        int2 size = restore ? m_restore_window_geometry.m_size : window.m_window_size;
        int2 position = restore ? m_restore_window_geometry.m_position : window.m_window_position;
        if (not restore and settings.m_window_mode == EDisplayWindowMode::windowed and m_windowed_geometry) {
            size = m_windowed_geometry->m_size;
            position = m_windowed_geometry->m_position;
        }
        if (settings.m_width != 0u) {
            size = int2{static_cast<int>(settings.m_width), static_cast<int>(settings.m_height)};
        } else if (settings.m_window_mode == EDisplayWindowMode::fullscreen) {
            size = selected->m_video_mode.m_resolution;
        }
        if (not restore and
            selected and
            (settings.m_monitor != "auto" or settings.m_window_mode == EDisplayWindowMode::fullscreen)) {
            position = selected->m_virtual_position;
        }
        // Mark before entering the operational boundary: even allocation failure
        // while collecting its result must not make a native mutation cancellable.
        m_display_controller.markWindowApplied();
        return service->configureDisplayWindow(
            window, settings.m_window_mode == EDisplayWindowMode::fullscreen ? &*selected : nullptr, position, size,
            settings.m_refresh_hz != 0u ? std::optional<u32>{settings.m_refresh_hz} : std::nullopt);
    }

    std::error_code Application::serviceDisplayTransaction_() {
        const EDisplayTransaction state = m_display_controller.transactionState();
        if (state == EDisplayTransaction::degraded) {
            return m_display_controller.transactionError();
        }
        if (state != EDisplayTransaction::unsubmitted and state != EDisplayTransaction::restore_unsubmitted) {
            return {};
        }
        if (m_display_transactions_stopped) {
            m_display_controller.cancelUnsubmitted();
            if (not m_display_controller.restoring()) {
                return {};
            }
        }
        if (m_pending_surface_lifetime != m_display_surface_lifetime) {
            m_display_controller.invalidatePending();
            return {};
        }

        // Drain old frame work through the existing ledger, never cancel an
        // accepted GPU submission or bypass its exactly-once settlement.
        reconcileSubmittedReservations_();
        if (m_frame_reservations.outstanding() != 0u) {
            return {};
        }

        const bool restore = m_display_controller.restoring();
        const DisplaySettings &settings = restore ? m_display_controller.committed()
                                                  : m_display_controller.pending()->m_resolved.m_value;
        if (restore) {
            // Restoration addresses the actual generation reported by failed apply.
            if (m_display_controller.applied().m_generation != kSurfaceGenerationNone) {
                m_pending_surface_identity.m_generation = m_display_controller.applied().m_generation;
            }
        }
        SurfaceSettings surface{.m_vsync = presentationVsync(settings)};
        if (m_pending_window_change) {
            if (not restore and m_window_operation_transaction == 0u) {
                const SurfaceGeneration current = renderLaneOwnsRenderer_()
                                                      ? laneSurfaceGeneration(m_pending_surface_identity.m_handle)
                                                      : m_renderer->surfaceGenerationFor(
                                                            m_pending_surface_identity.m_handle);
                if (current != m_pending_surface_identity.m_generation) {
                    m_display_controller.invalidatePending();
                    return {};
                }
            }
            const u64 transaction = m_display_controller.pending()->m_transaction_id;
            if (m_window_operation_transaction != transaction) {
                // Admission is suspended and the old reservation ledger drained.
                // OS work stays on the main/native thread; the renderer only sees
                // copied framebuffer geometry through the existing lane transport.
                m_window_operation_transaction = transaction;
                const auto geometry = applyDisplayWindow_(restore ? m_restore_window_settings : settings, restore);
                if (not geometry) {
                    m_display_controller.markSubmitted();
                    const DisplayAppliedState &observed = m_display_controller.applied();
                    AppliedSurfaceSettings actual{
                        m_pending_surface_identity.m_generation, m_restore_window_geometry.m_framebuffer_size,
                        observed.m_vsync, false, restore ? geometry.error() : std::error_code{}};
                    RenderSurfaceResult failed{transaction, m_pending_surface_identity, actual, geometry.error()};
                    return onSurfaceReconfigured_(failed);
                }
                m_pending_window_geometry = *geometry;
            }
            surface.m_extent = m_pending_window_geometry->m_framebuffer_size;
        }
        RenderSurfaceRequest request{
            m_display_controller.pending()->m_transaction_id, m_pending_surface_identity, surface};

        if (renderLaneOwnsRenderer_()) {
            const std::error_code error = m_render_lane->requestSurfaceReconfigure(request);
            if (error == std::make_error_code(std::errc::resource_unavailable_try_again) or
                error == std::make_error_code(std::errc::device_or_resource_busy)) {
                return {}; // Visibly unsubmitted; bounded retry at each pump.
            }
            if (error) {
                m_display_controller.markSubmitted();
                const DisplayAppliedState &observed = m_display_controller.applied();
                AppliedSurfaceSettings actual{
                    observed.m_generation != kSurfaceGenerationNone ? observed.m_generation
                                                                    : request.m_identity.m_generation,
                    int2{observed.m_width, observed.m_height}, observed.m_vsync, observed.m_available,
                    observed.m_restoration_error};
                RenderSurfaceResult failed{request.m_transaction_id, request.m_identity, actual, error};
                return onSurfaceReconfigured_(failed);
            }
            m_display_controller.markSubmitted();
            return {};
        }

        m_display_controller.markSubmitted();
        RenderSurfaceResult result{request.m_transaction_id, request.m_identity};
        const auto applied = m_renderer->reconfigureWindowSurface(
            request.m_identity.m_handle, request.m_identity.m_generation, request.m_settings);
        result.m_applied = m_renderer->appliedSurfaceSettingsFor(request.m_identity.m_handle);
        if (not applied) {
            result.m_error = applied.error();
        }
        return onSurfaceReconfigured_(result);
    }

    std::error_code Application::onSurfaceReconfigured_(const RenderSurfaceResult &result) {
        if (not m_display_controller.pending() or
            result.m_transaction_id != m_display_controller.pending()->m_transaction_id or
            result.m_identity.m_handle != m_pending_surface_identity.m_handle or
            result.m_identity.m_generation != m_pending_surface_identity.m_generation) {
            return {};
        }
        if (m_pending_surface_lifetime != m_display_surface_lifetime or
            result.m_identity.m_handle != m_display_surface_handle) {
            // Consume the old transaction without publishing any old-window
            // observation or pacing to the recreated window.
            m_display_controller.invalidatePending();
            return {};
        }

        const AppliedSurfaceSettings &applied = result.m_applied;
        const DisplaySettings &requested = m_display_controller.restoring()
                                               ? m_display_controller.committed()
                                               : m_display_controller.pending()->m_resolved.m_value;
        const bool valid_observation = applied.m_generation != kSurfaceGenerationNone and
                                       applied.m_vsync == presentationVsync(requested) and
                                       (not m_pending_window_change or
                                           not m_pending_window_geometry or
                                           (applied.m_extent.x == m_pending_window_geometry->m_framebuffer_size.x and
                                               applied.m_extent.y ==
                                                   m_pending_window_geometry->m_framebuffer_size.y)) and
                                       (applied.m_available or applied.m_extent.x <= 0 or applied.m_extent.y <= 0);
        const std::error_code error = result.m_error
                                          ? result.m_error
                                          : (valid_observation ? std::error_code{}
                                                               : std::make_error_code(std::errc::not_connected));
        const DisplayAppliedState observation{
            applied.m_generation, applied.m_extent.x, applied.m_extent.y, applied.m_vsync, applied.m_available,
            applied.m_restoration_error};
        const bool was_restoring = m_display_controller.restoring();
        if (not m_display_controller.acknowledge(result.m_transaction_id, observation, error)) {
            return {};
        }

        m_lane_surface_handle = result.m_identity.m_handle;
        m_lane_surface_generation = applied.m_generation;
        if (m_display_controller.transactionState() == EDisplayTransaction::idle) {
            if (not was_restoring) {
                const PacingTargets targets = pacingTargets(m_display_controller.committed());
                m_target_frame_time = targets.m_target;
                m_presentation_vsync = targets.m_vsync;
            }
            requestFrameAdmissionWake();
        }
        if (m_display_controller.transactionState() == EDisplayTransaction::degraded) {
            return m_display_controller.transactionError();
        }
        return {};
    }

    std::error_code Application::run() {
        // Exception safety net only: the fused finishRun_() below owns
        // normal-path cleanup, so this fires solely when an exception
        // escapes the loop body itself (best effort; the exception still
        // propagates after shutdown + drain are attempted).
        bool completed = false;
        PPR_DEFER {
            if (not completed) [[unlikely]] {
                std::ignore = shutdown();
                std::ignore = Log::flush(true);
            }
        };

        // B1'a epoch scope: a previous run() on this object leaves a
        // recorded (prepared) disposition behind. Clear it BEFORE
        // initialization can acquire new GPU resources so the coming
        // teardown independently re-establishes quiescence for this epoch's
        // submissions. Idempotence holds within the epoch only.
        resetGpuTeardownEpoch_();

        if (const std::error_code init_err = initialize()) {
            // B1'b: publish completion only after finishRun_ returns — an
            // exception inside finishRun_ (before proof) must reach the
            // nonthrowing termination path via the defer above, never skip
            // it as "already completed".
            const std::error_code result = finishRun_(init_err);
            completed = true;
            return result;
        }

        PPR_LOG(App, emphasis, "🏁 run application loop");

        // Run-loop invariant (A1 lesson): terminate on stable state — the
        // graceful-exit latch or the latched cancel error — never on the
        // consumable event pulse alone. run() reports the same stable error.
        while (not isExitRequested()) {
            const hal::ProfileScope scope{"App.Frame"};
            std::error_code first_err{};

            // Stage: combined admission (deadline AND credit in one helper:
            // wakeable pacer wait, then a zero-target tick for dt, then the
            // credit probe before any client work). A clock error OR a
            // latched exit retires the frame here: the error is latched for
            // run()'s return value and the loop leaves WITHOUT client or
            // render work on this path.
            TimeSpan dt{};
            bool admitted_skip = false;
            {
                const hal::ProfileScope scope_clock{"App.Frame.Clock"};
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, admitClientFrame_(&dt, admitted_skip));
            }

            if (first_err or isExitRequested()) {
                if (first_err) {
                    m_request_exit(first_err);
                }
                break;
            }

            // Admission + client stages share one exception boundary; every
            // handler retains the FIRST error, never overwriting it.
            // Per-frame submit record for the inline closure below: the
            // before-values prove whether THIS frame submitted, never a
            // sticky previous frame. Captured after admission, before any
            // client work; the closure compares after-values to these.
            // Untouched when a lane owns the renderer (main never reads
            // lane-owned state there).
            FenceCompletionValue submit_before{kFenceValueNone};
            FrameSettlement settlement_before{FrameSettlement::kRejected};
            bool inline_submit_observed = false;
            if (not renderLaneOwnsRenderer_() and m_renderer) {
                submit_before = m_renderer->lastSubmittedFenceValue();
                settlement_before = m_renderer->lastSubmitSettlement();
                inline_submit_observed = true;
            }
            try {
                // B2' admission outcome from admitClientFrame_ above (deadline
                // AND credit, before the whole virtual update stage including
                // derived pre-work). Terminal (quarantine included) already
                // retained first_err, latched it, and left the loop above.
                // Capacity-busy accumulates dt into the single
                // Application-owned carry, runs PLATFORM COLLECTION ONLY
                // (never update(dt): device time must not advance on a
                // skipped frame), latches a collection failure, and finishes
                // the iteration.
                if (admitted_skip) {
                    noteSkippedFrameDt(dt);
                    if (m_platform) {
                        // Single collection per busy frame through the
                        // wakeable wait: it stages arrivals itself (the
                        // default/scripted wait pumps once after the
                        // quantum; GLFW waits then collects), so a separate
                        // pump before it would collect twice per frame and
                        // shift pump-indexed accounting (B2' busy carry and
                        // the total-elapsed identity). Saturation otherwise
                        // loops while the 2-credit reservation bound holds.
                        // Elapsed time stays carried via noteSkippedFrameDt
                        // above; native collection/wait failures are
                        // retained, never ignored.
                        PPR_RETAIN_ERROR_ON_FAIL(
                            App, first_err, m_platform->waitNativeEvents(FramePacer::kWakeQuantum));
                    }
                    // Recheck completions so a credit freed during the wait
                    // is observed before the next probe: lane drain plus
                    // the inline retire/mirror when no lane owns the
                    // renderer, then reconcile. Errors retained with the
                    // same first-error rule; canceled stays neutral.
                    if (m_render_lane or m_display_controller.admissionSuspended()) {
                        if (const std::error_code busy_drain_err = pumpRenderCompletions_();
                            busy_drain_err and
                            not first_err and
                            busy_drain_err != std::make_error_code(std::errc::operation_canceled)) {
                            first_err = busy_drain_err;
                        }
                    }
                    if (not renderLaneOwnsRenderer_() and m_renderer) {
                        if (const std::error_code busy_retire_err = m_renderer->retireCompletedFrames();
                            busy_retire_err and
                            not first_err and
                            busy_retire_err != std::make_error_code(std::errc::operation_canceled)) {
                            first_err = busy_retire_err;
                        }
                        m_lane_retirement_frontier.store(m_renderer->retirementFrontier(), std::memory_order_release);
                    }
                    reconcileSubmittedReservations_();
                    if (first_err) [[unlikely]] {
                        m_request_exit(first_err);
                    }
                    ++m_frame_index;
                    continue;
                }

                // Admitted: prepend carried skipped-frame time exactly once
                // (exchange semantics inside claimSkippedFrameDt) so the
                // fixed-step sim observes preserved elapsed time.
                const TimeSpan carry = claimSkippedFrameDt();
                const TimeSpan effective_dt = dt + carry;
                std::error_code update_err{};
                {
                    const hal::ProfileScope scope_update{"App.Frame.Update"};
                    const hal::ProfileScope scope_frame_advance{"PPR.Frame.AdvanceClient"};
                    PPR_RETAIN_ERROR_ON_FAIL(App, first_err, update(effective_dt));
                    update_err = first_err;
                }


                // Render only when update succeeded: a failed update already
                // latched its error and the loop retires below.
                if (not update_err) {
                    const hal::ProfileScope scope_render{"App.Frame.Render"};
                    PPR_RETAIN_ERROR_ON_FAIL(App, first_err, render());
                }

            } catch (const std::system_error &e) {
                if (not first_err) {
                    first_err = e.code();
                }
            } catch (const std::invalid_argument &) {
                if (not first_err) {
                    first_err = std::make_error_code(std::errc::invalid_argument);
                }
            } catch (const std::bad_alloc &) {
                if (not first_err) {
                    first_err = std::make_error_code(std::errc::not_enough_memory);
                }
            } catch (...) {
                if (not first_err) {
                    first_err = std::make_error_code(std::errc::state_not_recoverable);
                }
            }

            // Inline settlement loop-closure: when no lane owns the
            // renderer, the frame's CPU settlement normally arrives via
            // the derived render path posting onLaneFrameConsumed_
            // (ApplicationEditor::submitRenderWork_'s inline branch). A
            // base-level render that never posts it would otherwise park
            // this frame's keyed reservation in kLocal forever —
            // occupancy never releases, admission stalls, and every busy
            // frame skips on a charge nobody can retire. Close it from a
            // per-frame before/after submit record, only where the client
            // left the record unsettled. A lane/editor settlement has
            // already moved the record out of kLocal and always wins.
            // An error never proves nothing submitted: an accepted submit
            // followed by a failed present advances the fence AND latches
            // first_err, so the submit fields are derived from fence
            // advancement (and a fresh kSubmitFailed flip) independently
            // of first_err; the error rides alongside, never instead.
            if (inline_submit_observed and m_renderer) {
                for (FrameReservation &record: m_reservations) {
                    if (not record.m_active or
                        record.m_state != FrameReservation::State::kLocal or
                        record.m_epoch != m_reservation_epoch or
                        record.m_frame_id != getFrameIndex()) {
                        continue;
                    }
                    RenderFrameResult synth{};
                    synth.m_frame_id = getFrameIndex();
                    synth.m_reservation_epoch = m_reservation_epoch;
                    synth.m_reservation_frame_id = getFrameIndex();
                    const FenceCompletionValue submit_after = m_renderer->lastSubmittedFenceValue();
                    const FrameSettlement fate_after = m_renderer->lastSubmitSettlement();
                    if (submit_after != kFenceValueNone and submit_after > submit_before) {
                        // This frame's submit was accepted (fence advanced),
                        // even when the present below it failed: the charge
                        // survives to GPU retirement via the settlement hook.
                        synth.m_submit_value = submit_after;
                        if (first_err) {
                            synth.m_error = first_err;
                        }
                    } else if (fate_after == FrameSettlement::kSubmitFailed and
                               settlement_before != FrameSettlement::kSubmitFailed) {
                        // This frame's submit possibly executed (no value
                        // committed, quarantine path): never a normal
                        // release, even with an error alongside.
                        synth.m_submit_possibly_executed = true;
                        if (first_err) {
                            synth.m_error = first_err;
                        }
                    } else {
                        // Definitely unsubmitted this frame: no fence
                        // advancement and no fresh submit failure. A sticky
                        // previous frame's fate is never consulted. A
                        // latched error here is a definite pre-submission
                        // failure and releases; clean success settles
                        // unsubmitted with no value and no error.
                        if (first_err) {
                            synth.m_error = first_err;
                        }
                    }
                    onLaneFrameConsumed_(synth);
                    break;
                }
            }

            if (first_err) [[unlikely]] {
                // Reservation accounting settles structurally at teardown:
                // definitely-unsubmitted local/queued records lose their
                // charge here; a submitted frame's charge survives to either
                // its GPU retirement or the teardown settle — never a
                // blanket counter reset that could duplicate or bury a credit.
                m_request_exit(first_err);
            }

            ++m_frame_index;
        }

        // Normal (non-error) run end: in-flight lane frames settle through
        // the same hook on their late results; the credit economy is rebuilt
        // by shutdown()'s settle phase. Scratch is cleared for reuse.
        m_render_result_scratch.clear();

        PPR_LOG(App, emphasis, "stop application loop, bye 👋");
        // Read the stable loop error BEFORE shutdown() resets the
        // lifecycle object that carries it; finishRun_() then retains the
        // first error across shutdown + logger drain.
        std::error_code loop_err{};
        if (m_lifecycle) {
            loop_err = m_lifecycle->error();
        }
        // B1'b: same ordering as the init-failure path above — the defer
        // stays armed until finishRun_ (proof + release) returns.
        const std::error_code result = finishRun_(loop_err);
        completed = true;
        return result;
    }

    void Application::noteSkippedFrameDt(const TimeSpan dt) noexcept {
        if (dt > TimeSpan{}) {
            m_pending_skipped_dt += dt;
        }
    }

    TimeSpan Application::claimSkippedFrameDt() noexcept {
        const TimeSpan carry = m_pending_skipped_dt;
        m_pending_skipped_dt = TimeSpan{};
        return carry;
    }

    std::error_code Application::admitClientFrame_(TimeSpan *out_delta_time, bool &out_skip) {
        // Single admission helper (deadline AND credit): the pacer deadline
        // half first, then the credit probe before any client work. The
        // ledger is the source of truth; pacer/lane signals are hints
        // rechecked here. Fail-closed like the loop boundary it replaces:
        // unexpected exceptions convert to error codes, never escape.
        out_skip = false;
        if (const std::error_code clock_err = advanceClock_(out_delta_time)) {
            return clock_err;
        }
        std::error_code admission_err{};
        try {
            admission_err = probeFrameAdmissionPreUpdate_(out_skip);
        } catch (const std::system_error &e) {
            admission_err = e.code();
        } catch (const std::invalid_argument &) {
            admission_err = std::make_error_code(std::errc::invalid_argument);
        } catch (const std::bad_alloc &) {
            admission_err = std::make_error_code(std::errc::not_enough_memory);
        } catch (...) {
            admission_err = std::make_error_code(std::errc::state_not_recoverable);
        }

        return admission_err;
    }

    void Application::waitForAdmissionHint_() {
        // Bounded backpressure wait after a transport drop: each slice
        // services platform events FIRST (admission never blocks the pump),
        // then parks natively. Hints only — every wake rechecks persistent
        // exit/terminal/credit state, and the budget caps the stall even if
        // no hint ever arrives. The budget is an ABSOLUTE deadline captured
        // once (kMaxHintSlices REQUESTED quanta of FramePacer::kWakeQuantum
        // — best-effort OS waits, no wall-clock guarantee): servicing
        // (drain/retire/reconcile/pump) runs first each slice, then only
        // the recomputed remainder (capped at one quantum) is waited — an
        // already-due budget returns immediately without waiting. Native
        // collection/wait failures and the completion drain are retained
        // durably into m_wait_pump_error (first error wins, canceled
        // neutral) instead of discarded, so they travel with the shutdown
        // result.
        constexpr int kMaxHintSlices{4};
        const TimePoint budget_deadline = time::now() + kMaxHintSlices * FramePacer::kWakeQuantum;
        m_admission_wake.resetEvent();
        for (int slice = 0; slice < kMaxHintSlices; ++slice) {
            if (const std::error_code hint_drain_err = pumpRenderCompletions_();
                hint_drain_err and
                not m_wait_pump_error and
                hint_drain_err != std::make_error_code(std::errc::operation_canceled)) {
                m_wait_pump_error = hint_drain_err;
                try {
                    PPR_LOG(
                        App, debug, "waitForAdmissionHint_:hint_drain retains wait_pump_error",
                        {{"category", hint_drain_err.category().name()}, {"value", hint_drain_err.value()},
                            {"origin", "waitForAdmissionHint_:hint_drain"}});
                } catch (...) {
                }
            }
            if (isExitRequested() or (m_render_lane and m_render_lane->terminalError())) {
                return;
            }
            // Credit release for accepted submissions is the AND of CPU
            // settlement and the published retirement frontier — recheck by
            // reconciling, not by guessing. Hold-sweep windows are never
            // reported as retirement, so they keep this hint armed. On the
            // inline path there is no lane sweep callback, so mirror the
            // frontier from the owner-thread renderer first — otherwise
            // the reconcile below can never retire an inline record.
            if (not renderLaneOwnsRenderer_() and m_renderer) {
                if (const std::error_code hint_retire_err = m_renderer->retireCompletedFrames();
                    hint_retire_err and
                    not m_wait_pump_error and
                    hint_retire_err != std::make_error_code(std::errc::operation_canceled)) {
                    m_wait_pump_error = hint_retire_err;
                    try {
                        PPR_LOG(
                            App, debug, "waitForAdmissionHint_:hint_retire retains wait_pump_error",
                            {{"category", hint_retire_err.category().name()}, {"value", hint_retire_err.value()},
                                {"origin", "waitForAdmissionHint_:hint_retire"}});
                    } catch (...) {
                    }
                }
                m_lane_retirement_frontier.store(m_renderer->retirementFrontier(), std::memory_order_release);
            }
            reconcileSubmittedReservations_();
            if (m_frame_reservations.outstanding() < kMaxOutstandingFrames) {
                return;
            }
            if (m_admission_wake.pollEvent()) {
                return;
            }
            if (m_platform) {
                if (const std::error_code hint_pump_err = m_platform->pumpNativeEvents();
                    hint_pump_err and
                    not m_wait_pump_error and
                    hint_pump_err != std::make_error_code(std::errc::operation_canceled)) {
                    m_wait_pump_error = hint_pump_err;
                    try {
                        PPR_LOG(
                            App, debug, "waitForAdmissionHint_:hint_pump retains wait_pump_error",
                            {{"category", hint_pump_err.category().name()}, {"value", hint_pump_err.value()},
                                {"origin", "waitForAdmissionHint_:hint_pump"}});
                    } catch (...) {
                    }
                }
                // Servicing above consumed budget: recompute the remainder
                // against the absolute deadline and never wait when due.
                const TimeSpan remaining = budget_deadline - time::now();
                if (remaining <= TimeSpan{}) {
                    return;
                }
                const TimeSpan quantum = remaining < FramePacer::kWakeQuantum ? remaining : FramePacer::kWakeQuantum;
                if (const std::error_code hint_wait_err = m_platform->waitNativeEvents(quantum);
                    hint_wait_err and
                    not m_wait_pump_error and
                    hint_wait_err != std::make_error_code(std::errc::operation_canceled)) {
                    m_wait_pump_error = hint_wait_err;
                    try {
                        PPR_LOG(
                            App, debug, "waitForAdmissionHint_:hint_wait retains wait_pump_error",
                            {{"category", hint_wait_err.category().name()}, {"value", hint_wait_err.value()},
                                {"origin", "waitForAdmissionHint_:hint_wait"}});
                    } catch (...) {
                    }
                }
            } else {
                return;
            }
        }
    }

    std::error_code Application::pumpRenderCompletions_() {
        const hal::ProfileScope scope_frame_retire{"PPR.Frame.Retire"};
        if (not m_render_lane) {
            return serviceDisplayTransaction_();
        }
        m_render_result_scratch.clear();
        // Nonblocking drain. The error is retained, never discarded: it
        // duplicates the first failed result's m_error when stash/transport
        // loss occurred, and stands alone otherwise. Every completable
        // outcome and every accounting side effect below runs regardless.
        const std::error_code drain_error = m_render_lane->drainResults(m_render_result_scratch);
        std::error_code first_err{};
        for (const RenderFrameResult &result: m_render_result_scratch) {
            if (result.m_surface_result) {
                const std::error_code surface_error = onSurfaceReconfigured_(*result.m_surface_result);
                if (surface_error and not first_err) {
                    first_err = surface_error;
                }
                continue; // Control completion owns no credit or staging ack.
            }
            // Surface-generation mirror (hint): the lane re-verifies every
            // capture claim against the live record at submit.
            if (result.m_surface_handle and
                (not m_display_surface_handle or result.m_surface_handle == m_display_surface_handle) and
                result.m_surface_generation >= m_lane_surface_generation) {
                m_lane_surface_handle = result.m_surface_handle;
                m_lane_surface_generation = result.m_surface_generation;
            }
            // Every known outcome fires the settlement contract, including
            // dropped/cancelled slots; both milestones (this hook + the
            // ledger's fence retirement) are durable and independent.
            onLaneFrameConsumed_(result);
            // operation_canceled is a neutral teardown artifact for the
            // transport (dropped at stop/drain), not an app-level failure:
            // the settlement hook already re-armed the owner's flags. A
            // frame error outranks its own sweep error: per-slot first-error
            // precedence keeps the originating failure authoritative while
            // the retirement failure is never silently dropped.
            if (result.m_error and
                not first_err and
                result.m_error != std::make_error_code(std::errc::operation_canceled)) {
                first_err = result.m_error;
            } else if (result.m_sweep_error and
                       not first_err and
                       result.m_sweep_error != std::make_error_code(std::errc::operation_canceled)) {
                first_err = result.m_sweep_error;
            }
        }
        m_render_result_scratch.clear();
        if (const std::error_code transaction_error = serviceDisplayTransaction_();
            transaction_error and not first_err) {
            first_err = transaction_error;
        }
        // Retained drain/transport failure joins with the same precedence
        // (result errors seen above win; canceled is neutral).
        if (not first_err and drain_error and drain_error != std::make_error_code(std::errc::operation_canceled)) {
            first_err = drain_error;
        }
        // Final-sweep outcome is the authoritative completion of the
        // lane's retirement flood: retain it even when the steady
        // per-frame sweeps already failed (first error preserved), and
        // never skip it — a sticky lane-report surfaces through the same
        // single completion transport, never a forked mechanism.
        if (not first_err) {
            if (const std::error_code final_err = m_render_lane->finalSweepError();
                final_err and final_err != std::make_error_code(std::errc::operation_canceled)) {
                first_err = final_err;
                try {
                    PPR_LOG(
                        App, debug, "pumpRenderCompletions_:finalSweep retains final_err",
                        {{"category", final_err.category().name()}, {"value", final_err.value()},
                            {"origin", "pumpRenderCompletions_:finalSweep"}});
                } catch (...) {
                }
            }
        }
        return first_err;
    }

    void Application::onLaneFrameConsumed_(const RenderFrameResult &result) {
        // Durable settlement reconciliation: match the posted result to
        // exactly one live reservation record by (epoch, frame id). A
        // stale/foreign-epoch result never releases a current-run credit,
        // and neither does one with no recorded seat.
        if (result.m_reservation_epoch != m_reservation_epoch) {
            return;
        }
        const auto matches = [&result](const FrameReservation &record) noexcept {
            return record.m_active and
                   record.m_epoch == result.m_reservation_epoch and
                   record.m_frame_id == result.m_frame_id;
        };
        auto it = std::find_if(m_reservations.begin(), m_reservations.end(), matches);
        if (it == m_reservations.end()) {
            // No matching live record: the outcome is for a frame whose
            // admission was abandoned (error paths that abandon with
            // first_err — see run loop), or a frame the ledger already
            // accounted for. Nothing to reconcile.
            return;
        }
        FrameReservation &record = *it;
        if (result.m_submit_possibly_executed) {
            // FrameSettlement::kSubmitFailed: admission is stopped and the
            // credit is never reused — quarantined, stays charged.
            record.m_state = FrameReservation::State::kQuarantined;
            record.m_cpu_settled = true;
            return;
        }
        if (result.m_submit_value != kFenceValueNone) {
            // An accepted submission was made — the charge holds until the
            // retirement frontier covers this value. Even a later present
            // error riding on this result does NOT reclaim the slot: the
            // submission was really accepted, so the CPU-side reservation
            // must follow GPU retirement rather than present status.
            const FenceCompletionValue frontier = m_lane_retirement_frontier.load(std::memory_order_acquire);
            record.m_state = FrameReservation::State::kSubmitted;
            record.m_submit_value = result.m_submit_value;
            record.m_cpu_settled = true;
            if (frontier >= result.m_submit_value) {
                // Accepted AND already covered by the retirement frontier
                // (inline success on a frame retired within the same sweep,
                // or a same-value must-publish match): retire once now.
                releaseReservation_(record);
            }
            return;
        }
        if (result.m_dropped) {
            // Never-applied slot (lane drop, transport rejection, obsolete
            // generation sizing, minimized surface): the runtime never has
            // to retire a GPU value for it — release the credit now.
            releaseReservation_(record);
            return;
        }
        if (result.m_error) {
            // Definitely unsubmitted failure (transport rejection, definite
            // pre-submission rejection): release now.
            releaseReservation_(record);
            return;
        }
        // No accepted submission, no error: settled-but-unsubmitted frame.
        releaseReservation_(record);
    }

    void Application::publishLaneRetirementFrontier_() {
        // Null-guarded one-line mirror: a torn-down renderer publishes
        // nothing, never a manufactured value. Release pairs with main's
        // acquire in reconcileSubmittedReservations_() and
        // onLaneFrameConsumed_().
        if (m_renderer) {
            m_lane_retirement_frontier.store(m_renderer->retirementFrontier(), std::memory_order_release);
        }
    }

    void Application::reconcileSubmittedReservations_() {
        const FenceCompletionValue frontier = m_lane_retirement_frontier.load(std::memory_order_acquire);
        for (FrameReservation &record: m_reservations) {
            if (not record.m_active or record.m_state != FrameReservation::State::kSubmitted) {
                continue;
            }
            if (record.m_cpu_settled and
                record.m_submit_value != kFenceValueNone and
                frontier >= record.m_submit_value) {
                releaseReservation_(record);
            }
        }
        std::erase_if(m_reservations, [](const FrameReservation &record) { return not record.m_active; });
    }

    void Application::noteReservationQueued_(const u64 frame_id) {
        for (FrameReservation &record: m_reservations) {
            if (record.m_active and record.m_epoch == m_reservation_epoch and record.m_frame_id == frame_id) {
                record.m_state = FrameReservation::State::kQueued;
                return;
            }
        }
    }

    void Application::releaseReservation_(FrameReservation &record) {
        // Release the credit exactly once; a record that was never
        // charged (std::error inside acquire paths) never reaches here.
        // An empty-ledger release here is a bug — keep it loud, never
        // silently dropped (ADR Ora-6 item 3).
        record.m_active = false;
        if (const std::error_code release_err = m_frame_reservations.release()) {
            PPR_ASSERT(not release_err && "reservation release hit an empty ledger");
        }
    }

    void Application::installPacerSleepOnce_() {
        // Installed once: every pacer sleep slice pumps native events, then
        // waits natively for the slice remainder — waits stay event-driven
        // (exit/credit/config wakes cut them short) without spinning. Each
        // slice duration is a REQUESTED quantum (FramePacer::kWakeQuantum):
        // a best-effort OS/native wait that may return early on a wake or
        // late on scheduling granularity — never an exact wall-clock
        // guarantee. The absolute deadline is owned per sleep request in
        // pacerSleepSlice_ below.
        if (m_pacer_sleep_installed) {
            return;
        }
        m_pacer_sleep_installed = true;
        m_pacer.setSleepFunction([this](const TimeSpan slice) noexcept { pacerSleepSlice_(slice); });
    }

    void Application::pacerSleepSlice_(const TimeSpan slice) noexcept {
        // Every pacer sleep slice pumps then waits natively so waits stay
        // event-driven. The incoming slice is a REQUESTED quantum
        // (FramePacer::kWakeQuantum, 2ms): best-effort, no wall-clock
        // guarantee. The ABSOLUTE deadline (entry now + slice) is captured
        // once per sleep request and retained through servicing: the
        // pump/collect runs first, then the remainder (deadline minus a
        // fresh now) is recomputed — only the remainder is waited, and an
        // already-due deadline returns immediately without waiting.
        // Platform errors are retained durably into m_wait_pump_error
        // (first error wins, canceled neutral) instead of discarded; they
        // travel with the shutdown result via stopRenderLane_(). Noexcept:
        // retention is assignment-only, never throwing.
        try {
            const TimePoint deadline = time::now() + slice;
            if (m_platform) {
                const hal::ProfileScope scope_frame_service{"PPR.Frame.ServicePlatform"};
                std::ignore = pumpOperationBestEffort_(
                    [this] { return m_platform->pumpNativeEvents(); }, m_wait_pump_error);
                // A long old CPU cap must not defer a display acknowledgement
                // until another admitted frame. Retargeting still uses the
                // existing wakeable pacer and its original tick anchor.
                if (m_display_controller.admissionSuspended()) {
                    if (const std::error_code completion_error = pumpOperationBestEffort_(
                            [this] { return pumpRenderCompletions_(); }, m_wait_pump_error);
                        completion_error and completion_error != std::make_error_code(std::errc::operation_canceled)) {
                        if (not m_wait_pump_error) {
                            m_wait_pump_error = completion_error;
                        }
                        requestExit(m_wait_pump_error);
                    }
                }
                const TimeSpan remaining = deadline - time::now();
                if (remaining <= TimeSpan{}) {
                    return;
                }
                std::ignore = pumpOperationBestEffort_(
                    [this, remaining] { return m_platform->waitNativeEvents(remaining); }, m_wait_pump_error);
            } else {
                const TimeSpan remaining = deadline - time::now();
                if (remaining <= TimeSpan{}) {
                    return;
                }
                std::this_thread::sleep_for(remaining);
            }
        } catch (...) {
            const std::error_code error = applicationExceptionError_();
            if (not m_wait_pump_error and error != std::make_error_code(std::errc::operation_canceled)) {
                m_wait_pump_error = error;
            }
        }
    }

    bool Application::renderLaneOwnsRenderer_() const noexcept {
        // Ownership, not liveness: the lane owns Renderer affinity from
        // startRenderLane_ until the lane object is destroyed, independent
        // of the transient isRunning() pulse. Affinity decisions (which
        // lane may touch Renderer/pass-GPU state) consult ONLY this
        // stable ownership; isRunning() is a liveness hint for handoff
        // willingness, never permission to migrate execution to main. A
        // stopped-but-owned lane still owns affinity — its handoffs fail
        // closed (unacknowledged) and route to UNKNOWN, never to an inline
        // fallback that would touch owned state off-lane.
        return m_render_lane != nullptr and m_renderer != nullptr;
    }

    std::error_code Application::startRenderLane_() try {
        // The lane owns Renderer affinity from here on: init runs ON the
        // lane (owner id never migrates), sweeps run per frame on it, and
        // the wake callback relays hints to main. The frame executor is
        // bound later by the derived host (passes exist only after its
        // initialize); unbound frames fail closed until then.
        PPR_ASSERT(m_renderer != nullptr);
        PPR_ASSERT(not renderLaneOwnsRenderer_());
        auto lane = std::make_unique<RenderLane>();
        RenderLane *const lane_view = lane.get();
        lane_view->setSurfaceExecutor([this](const RenderSurfaceRequest &request, RenderSurfaceResult &result) {
            if (not m_renderer) {
                result.m_error = std::make_error_code(std::errc::not_connected);
                return;
            }
            const auto applied = m_renderer->reconfigureWindowSurface(
                request.m_identity.m_handle, request.m_identity.m_generation, request.m_settings);
            result.m_applied = m_renderer->appliedSurfaceSettingsFor(request.m_identity.m_handle);
            if (not applied) {
                result.m_error = applied.error();
            }
        });
        RenderLaneSweep sweep{[this](u32 &out_outstanding) -> std::error_code {
            // Fail-closed, never teardown-abort: the lane can still issue a
            // settle sweep during stop+join ordering gaps; a null renderer
            // must degrade to an error code, not an assert.
            if (not m_renderer) [[unlikely]] {
                out_outstanding = 0u;
                return std::make_error_code(std::errc::state_not_recoverable);
            }
            const std::error_code sweep_err = m_renderer->retireCompletedFrames();
            out_outstanding = m_renderer->outstandingFrameCount();
            // Publish the retirement frontier with release so main can
            // reconcile (acquire) its submitted-reservation records against
            // actually-retired fence values, advanced only on successful
            // sweep — never from a failed poll or removal.
            publishLaneRetirementFrontier_();
            return sweep_err;
        }};
        RenderLaneWake wake{[this]() noexcept { requestFrameAdmissionWake(); }};
        safe_ptr<IRhiService> rhi_service{getServices().get<IRhiService>()};
        // Publish ownership before thread startup. Every later failure must stop/join the owned lane.
        m_render_lane = std::move(lane);
        if (const std::error_code start_err = lane_view->start(
                [this, rhi_service]() -> std::error_code {
                    PPR_ASSERT(m_renderer != nullptr);
                    if (not rhi_service.isValid()) [[unlikely]] {
                        return std::make_error_code(std::errc::not_connected);
                    }
                    return m_renderer->initialize(*rhi_service);
                },
                RenderFrameExecutor{}, std::move(sweep), std::move(wake))) {
            std::ignore = stopRenderLane_();
            return start_err;
        }
        // P2 teardown protocol: while the caller waits on a sync
        // handoff or a lane join, main keeps the platform service
        // alive. Mirror waitForAdmissionHint_: completions first, then
        // collection-only native pump, then a bounded native wait
        // slice. Never blocks the lane; failures are retained by the
        // pump completion contract, never thrown across the slice.
        // Pump-compatible join: the wait pump installed at
        // startRenderLane_ services platform events while the
        // lane quiesces. A timeout escalates to the UNKNOWN
        // process policy: no live resource is freed (the lane,
        // its transports, and the renderer stay owned), the
        // pump stops, and the latched first/terminal error is
        // handed to the fail-stop boundary.
        m_render_lane->setWaitPump([this]() noexcept {
            // Each phase retains its error before proceeding; diagnostics cannot suppress later servicing.
            std::ignore = pumpOperationBestEffort_([this] { return pumpRenderCompletions_(); }, m_wait_pump_error);
            if (m_platform) {
                std::ignore = pumpOperationBestEffort_(
                    [this] { return m_platform->pumpNativeEvents(); }, m_wait_pump_error);
                std::ignore = pumpOperationBestEffort_(
                    [this] { return m_platform->waitNativeEvents(FramePacer::kWakeQuantum); }, m_wait_pump_error);
            }
        });
        return default_value_v;
    } catch (...) {
        const std::error_code error = applicationExceptionError_();
        if (m_render_lane) {
            std::ignore = stopRenderLane_();
        }
        return error;
    }

    std::error_code Application::stopRenderLane_() noexcept {
        // Teardown protocol tail: admission already stopped (quarantine in
        // prepareGpuTeardown_) and exit already signalled; settle + join
        // here while the platform still pumps, then destroy the transports
        // after the last emitter is gone.
        std::error_code lane_err = m_wait_pump_error;
        if (m_render_lane) {
            try {
                m_display_transactions_stopped = true;
                m_display_controller.stopTransactions();
                m_render_lane->requestStop();
                if (const std::error_code join_err = m_render_lane->join(); join_err) {
                    failStopOnUnknownGpuTeardown_(join_err);
                }
            } catch (...) {
                // No proven join: never reset ownership or free transports behind potentially live work.
                failStopOnUnknownGpuTeardown_(applicationExceptionError_());
            }
            if (not lane_err) {
                lane_err = m_wait_pump_error;
            }
            // Unconditional final settle BEFORE the transports die and
            // accounting resets: drain every remaining settlement and
            // surface the lane's final sweep status even when the join
            // already latched a timeout. Raw channels run no element
            // dtors, so this drain is also what retires the last live
            // RenderFrameResult/shared_ptr bytes.
            try {
                if (const std::error_code drain_err = pumpRenderCompletions_(); drain_err) {
                    if (not lane_err) {
                        lane_err = drain_err;
                    }
                    try {
                        PPR_LOG(
                            App, info, "stopRenderLane_:drain retains drain_err",
                            {{"category", drain_err.category().name()}, {"value", drain_err.value()},
                                {"origin", "stopRenderLane_:drain"}});
                    } catch (...) {
                        // Reporting cannot change retained error or bypass independent cleanup after join.
                    }
                }
            } catch (...) {
                if (not lane_err) {
                    lane_err = applicationExceptionError_();
                }
            }
            m_render_lane.reset();
        }
        if (m_wait_pump_error) {
            try {
                PPR_LOG(
                    App, info, "stopRenderLane_:wait_pump_error joins lane_err",
                    {{"category", m_wait_pump_error.category().name()}, {"value", m_wait_pump_error.value()},
                        {"origin", "stopRenderLane_:wait_pump_error"}});
            } catch (...) {
            }
            lane_err = lane_err ? lane_err : m_wait_pump_error;
        }
        return lane_err;
    }

    std::error_code Application::probeFrameAdmissionPreUpdate_(bool &out_skip) {
        out_skip = false;
        if (not m_domain.m_needs_rendering or not m_renderer) {
            return default_value_v;
        }
        if (renderLaneOwnsRenderer_()) {
            // Lane branch: drain completions (a failed frame's error fails
            // the app — it already retired lane-side), honor the latched
            // terminal, and apply the reservation bound before any client
            // construction. Busy never fails the app: skip the frame with
            // its dt carried. Capacity-busy stays the single skippable
            // outcome; the acquired credit is held from BEFORE frame
            // construction through the settlement hook, so a third frame
            // can never be constructed while two are outstanding.
            if (const std::error_code completion_err = pumpRenderCompletions_()) {
                return completion_err;
            }
            if (const std::error_code terminal_err = m_render_lane->terminalError()) {
                return terminal_err;
            }
            // Fix-19 stop-admission-on-exit (P0): the drain above runs the
            // settlement hook, which may have requested exit (the lane-gate
            // settlement ends the run); a same-iteration acquire after that
            // would construct a new frame on an exit-requested run. Refuse
            // the credit here so exit requested during drain/settlement
            // stops the next admission in the same iteration. First-error
            // preserved: drain/terminal errors already returned above.
            if (isExitRequested()) {
                out_skip = true;
                return default_value_v;
            }
            // Reconcile FIRST: a CPU-settled/GPU-pending record only leaves
            // the ledger here, so the capacity gate must run after the drain,
            // never before it (otherwise it can never release on the threaded
            // path, which emits no other reconcile trigger per frame).
            reconcileSubmittedReservations_();
            if (m_display_controller.admissionSuspended()) {
                out_skip = true;
                return default_value_v;
            }
            if (m_frame_reservations.outstanding() >= kMaxOutstandingFrames) {
                // The frontier mirror may have retired a still-charged record
                // above. Only the settled/retired half leaves — a merely
                // CPU-consumed record NEVER reopens capacity.
                out_skip = true;
                return default_value_v;
            }
            if (const std::error_code acquire_err = m_frame_reservations.acquire();
                acquire_err == std::make_error_code(std::errc::device_or_resource_busy)) {
                out_skip = true;
                return default_value_v;
            } else if (acquire_err) [[unlikely]] {
                return acquire_err;
            }
            // Keyed record replaces the lane pending counter: the credit
            // stays charged from BEFORE construction through CPU settlement
            // AND (for accepted submissions) GPU retirement.
            FrameReservation &reservation = [this]() -> FrameReservation & {
                try {
                    return m_reservations.emplace_back();
                } catch (...) {
                    std::ignore = m_frame_reservations.release();
                    throw;
                }
            }();
            reservation.m_active = true;
            reservation.m_epoch = m_reservation_epoch;
            reservation.m_frame_id = getFrameIndex();
            reservation.m_state = FrameReservation::State::kLocal;
            return default_value_v;
        }
        if (const std::error_code retire_err = m_renderer->retireCompletedFrames()) {
            // Terminal precedence: an abandoned/lost fence reports its poll
            // failure here, but the pipeline fate is device loss — report
            // the terminal code so producers route to teardown/recovery,
            // never mistaking it for a transient failure. Any other poll
            // error propagates as-is (never invented completion, never
            // capacity-busy).
            if (const std::error_code probe_err = m_renderer->checkFrameAdmission();
                probe_err == std::make_error_code(std::errc::no_such_device_or_address)) {
                return probe_err;
            }
            return retire_err;
        }
        if (const std::error_code probe_err = m_renderer->checkFrameAdmission()) {
            if (probe_err == std::make_error_code(std::errc::device_or_resource_busy)) {
                out_skip = true;
                return default_value_v;
            }
            return probe_err;
        }
        // Inline branch: the renderer's own ledger is the GPU-side truth,
        // but construction is gated by the same main-side reservation so a
        // third frame is never built while two are outstanding.
        // Mirror the retirement frontier from the sweep just above, then let
        // the settled/retired half leave before checking the bound. The
        // frontier never manufactures values (hold seam/failed poll/removal
        // all keep it where it last proved a real one).
        m_lane_retirement_frontier.store(m_renderer->retirementFrontier(), std::memory_order_release);
        reconcileSubmittedReservations_();
        if (const std::error_code transaction_error = serviceDisplayTransaction_()) {
            return transaction_error;
        }
        if (m_display_controller.admissionSuspended()) {
            out_skip = true;
            return default_value_v;
        }
        if (m_frame_reservations.outstanding() >= kMaxOutstandingFrames) {
            out_skip = true;
            return default_value_v;
        }
        if (const std::error_code acquire_err = m_frame_reservations.acquire();
            acquire_err == std::make_error_code(std::errc::device_or_resource_busy)) {
            out_skip = true;
            return default_value_v;
        } else if (acquire_err) [[unlikely]] {
            return acquire_err;
        }
        // Same keyed record as the lane branch: the credit releases only
        // after CPU settlement AND the matching GPU retirement.
        FrameReservation &reservation = [this]() -> FrameReservation & {
            try {
                return m_reservations.emplace_back();
            } catch (...) {
                std::ignore = m_frame_reservations.release();
                throw;
            }
        }();
        reservation.m_active = true;
        reservation.m_epoch = m_reservation_epoch;
        reservation.m_frame_id = getFrameIndex();
        reservation.m_state = FrameReservation::State::kLocal;
        return default_value_v;
    }

    std::error_code Application::advanceClock_(
        TimeSpan *out_delta_time) { // P0 serial-explicit pacing: wakeable external waits against the
        // previous-tick-anchored deadline (min-period policy, no catch-up),
        // then the manager ticks with a zero target so now() stays latched
        // and only callback dispatch + dt accounting run. Focus/background/
        // uncapped transitions re-evaluate here every frame via
        // getTargetFrameTime() (Phase 3 DisplayController overlay: §4.1
        // foreground mapping plus the §4.2 unfocused floor). Each wait slices at the wake quantum and the
        // predicate covers the persistent exit/error latch plus a sampled
        // target change, so a long background wait wakes within one quantum
        // on exit or retarget; a retarget loops to recompute the deadline
        // from the same previous tick before admission.
        for (;;) {
            const std::optional<TimeSpan> target = getTargetFrameTime();
            const TimeSpan period = target.value_or(TimeSpan{});
            const TimePoint previous_tick = m_application_clock.now();
            if (period <= TimeSpan{}) {
                break;
            }
            if (not m_pacer.shouldWait(FramePacer::deadlineFor(previous_tick, period))) {
                break;
            }
            m_pacer.waitForTarget(period, previous_tick, [this, target]() noexcept -> bool {
                if (isExitRequested()) {
                    return true;
                }
                return getTargetFrameTime() != target;
            });
            if (isExitRequested()) {
                break;
            }
            if (getTargetFrameTime() != target) {
                continue;
            }
            break;
        }
        return m_application_clock.tick(out_delta_time, TimeSpan{});
    }

    std::error_code Application::finishRun_(const std::error_code prior) {
        std::error_code result = prior;
        // B1': establish GPU quiescence BEFORE the virtual shutdown() below
        // so every release path runs under a recorded disposition. Retain:
        // an UNKNOWN here never overwrites the caller's original error —
        // both are preserved (original in `result` first, shutdown errors
        // retained after), and the fail-stop inside shutdown() still fires.
        // Log-free like the shutdown entries: formatting the quiescence
        // error here could throw before the boundary below settles.
        if (const std::error_code teardown_err = prepareGpuTeardown_()) {
            if (not result) {
                result = teardown_err;
            }
        }
        PPR_RETAIN_ERROR_ON_FAIL(App, result, shutdown());

        // Synchronous exit-time drain: the logger worker is async and
        // immediate categories self-flush, so only shutdown waits.
        // Retain a drain failure instead of silently ignoring it.
        PPR_RETAIN_ERROR_ON_FAIL(App, result, Log::flush(true));

        if (result) [[unlikely]] {
            m_request_exit(result);
        }
        return result;
    }

    std::error_code Application::initialize() {
        const hal::ProfileScope scope{"App.Initialize"};
        // B1'a: fresh epoch BEFORE any resource-creation entry below
        // (platform, shader, RHI, renderer). Covers direct initialize() /
        // partial-init rollback paths; run() re-arms per epoch as well.
        resetGpuTeardownEpoch_();
        m_application_clock.reset();
        // P2 pacer seam (once): waits stay event-driven from here on.
        installPacerSleepOnce_();

        PPR_LOG(
            App, info, "starting application",
            {
                {"name", m_name},
                {"platform", hal::platformName()},
                {"args", opaqueValue(m_arguments)},
            });

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->initialize(*this));

        // The defer only rolls back the lifecycle pair and window handles when
        // the state machine never reaches initialized; owned service
        // registrations are torn down explicitly via Application::shutdown()
        // on the init-failure path below.
        bool success = false;
        PPR_DEFER {
            if (not success) [[unlikely]] {
                std::ignore = Application::shutdown();
            }
        };

        m_content_dir = std::filesystem::directory_entry(hal::process::currentExecutablePath().parent_path());
        m_working_dir = std::filesystem::directory_entry(std::filesystem::current_path());
        m_install_dir = m_content_dir;

        std::tie(m_lifecycle, m_request_exit) = context::withCancelClause(context::background());

        // Re-arm the graceful-exit latch for repeated run() invocations.
        m_exit_requested.store(false, std::memory_order_release);

        context::setApplication(m_lifecycle);

        if (m_domain.m_needs_rendering) {
            const safe_ptr<IShaderService> shader_service = IShaderService::get();
            {
                const hal::ProfileScope scope_shader{"App.Initialize.Shader"};
                PPR_RETURN_ERROR_ON_FAIL(App, shader_service->initialize());
            }
            m_services.insert_or_assign(shader_service);

            const safe_ptr<IRhiService> rhi_service = IRhiService::get();
            {
                const hal::ProfileScope scope_rhi{"App.Initialize.RHI"};
                PPR_RETURN_ERROR_ON_FAIL(App, rhi_service->initialize(rhi::DeviceType::Default, *shader_service));
            }
            m_services.insert_or_assign(rhi_service);

            auto renderer = std::make_unique<Renderer>(SurfaceSettings{.m_vsync = m_presentation_vsync});
            // Committed startup presentation settings cross the ownership
            // boundary by value. Later edits use the acknowledged transaction.
            if (m_render_lane_enabled and not m_domain.m_is_headless) {
                // P2: the Renderer initializes ON the dedicated lane, so
                // its owner id is the lane from birth and never migrates.
                // All later Renderer/pass-GPU calls run there; main
                // coordinates via the lane transports only. The init-failure
                // defer below rolls back through shutdown(), which stops a
                // half-started lane before services die.
                m_renderer = std::move(renderer);
                {
                    const hal::ProfileScope scope_renderer{"App.Initialize.Renderer"};
                    PPR_RETURN_ERROR_ON_FAIL(App, startRenderLane_());
                }
            } else {
                // Inline reference (headless/offscreen/scripted): identical
                // stages, same thread, no transport.
                {
                    const hal::ProfileScope scope_renderer{"App.Initialize.Renderer"};
                    PPR_RETURN_ERROR_ON_FAIL(App, renderer->initialize(*rhi_service));
                }
                m_renderer = std::move(renderer);
            }
        }

        success = true;
        return default_value_v;
    }

    std::error_code Application::shutdown() {
        PPR_DEFER {
            context::setApplication(SharedContext{});

            m_lifecycle.reset();
            m_request_exit.reset();
        };

        std::error_code first_err{};

        // B1' fail-stop boundary: preparation + the UNKNOWN check run with
        // NO logging macros. PPR_RETAIN_ERROR_ON_FAIL formats via allocating
        // error_code::message(), and a throw there would escape shutdown
        // without _Exit(0xB1). Retain silently; diagnostics wait until the
        // disposition below authorizes release. The catch-all hardens the
        // UNKNOWN boundary itself: no exception between classification and
        // the fail-stop may skip _Exit.
        try {
            if (const std::error_code teardown_err = prepareGpuTeardown_()) {
                if (not first_err) {
                    first_err = teardown_err;
                }
            }
            if (m_gpu_teardown_disposition == GpuQuiescence::kUnknown) {
                failStopOnUnknownGpuTeardown_(m_gpu_teardown_error);
            }
        } catch (...) {
            failStopOnUnknownGpuTeardown_(m_gpu_teardown_error);
        }

        // Safe paths only from here: quiescence is proven idle or removal
        // is confirmed, so entry/release diagnostics may log again. The
        // log below is best-effort: a throwing diagnostic must never skip
        // the lane stop/join and renderer release that follow.
        try {
            PPR_LOG(
                App, info, "shutdown:quiescence disposition entering teardown",
                {{"disposition", static_cast<int>(m_gpu_teardown_disposition)},
                    {"category", m_gpu_teardown_error.category().name()}, {"value", m_gpu_teardown_error.value()},
                    {"origin", "shutdown:quiescence"}});
            PPR_LOG(App, info, "shut down application", {{"name", m_name}, {"platform", hal::platformName()}});
        } catch (...) {
        }

        if (m_renderer) {
            // P2 teardown order: the Renderer shuts down ON its owner lane
            // (quiescence was established without releasing; release runs
            // only under a safe disposition), then the lane stops and joins
            // while the platform still pumps, before RHI/services die below.
            if (renderLaneOwnsRenderer_()) {
                std::error_code renderer_err{};
                bool renderer_acknowledged = false;
                try {
                    renderer_err = m_render_lane->runOnLaneSync(
                        [this]() -> std::error_code {
                            PPR_ASSERT(m_renderer != nullptr);
                            return m_renderer->shutdown();
                        },
                        &renderer_acknowledged);
                } catch (...) {
                    renderer_err = std::make_error_code(std::errc::state_not_recoverable);
                }
                // Unacknowledged completion releases nothing: the lane may
                // still run the shutdown concurrently, so fail-stop before
                // the release below. Silent retain — diagnostics wait until
                // the disposition authorizes release.
                if (not renderer_acknowledged) {
                    failStopOnUnknownGpuTeardown_(renderer_err);
                }
                if (renderer_err and not first_err) {
                    first_err = renderer_err;
                }
            } else {
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_renderer->shutdown());
            }
            // Best-effort stage record (noexcept, never allocates past the
            // helper's own guard): a throwing append here must not skip the
            // stopRenderLane_() join and renderer release below.
            noteGpuTeardownStageBestEffort_("renderer");
        }
        // The worker is gone BEFORE the renderer it references is released:
        // the sweep/executor lambdas capture `this` and dereference
        // m_renderer, so any lane iteration running between release and
        // stop would assert-fire / touch freed GPU state. Stop+join first,
        // then release the renderer, then RHI/shader/platform teardown.
        PPR_RETAIN_ERROR_ON_FAIL(App, first_err, stopRenderLane_());
        if (m_renderer) {
            m_renderer.reset();
        }
        // Reservation-economy teardown: clear records, reset the main
        // ledger and the frontier mirror only NOW — after admission stopped,
        // every GPU-pending submission has been retired (or its loss
        // confirmed), and the lane's results have all drained. Epoch bumps
        // on the next run()/initialize() iff everything has settled.
        m_reservations.clear();
        m_frame_reservations.reset();
        m_lane_retirement_frontier.store(kFenceValueNone, std::memory_order_release);

        if (m_domain.m_needs_rendering) {
            const safe_ptr<IRhiService> rhi_service = IRhiService::get();
            if (m_services.tryGet<IRhiService>() == rhi_service) {
                const std::error_code shutdown_error = rhi_service->shutdown();
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, shutdown_error);
                if (not shutdown_error) {
                    PPR_VERIFY(m_services.erase(*rhi_service));
                }
            }
            noteGpuTeardownStageBestEffort_("rhi");

            const safe_ptr<IShaderService> shader_service = IShaderService::get();
            if (m_services.tryGet<IShaderService>() == shader_service) {
                const std::error_code shutdown_error = shader_service->shutdown();
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, shutdown_error);
                if (not shutdown_error) {
                    PPR_VERIFY(m_services.erase(*shader_service));
                }
            }
        }

        if (m_platform) {
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_platform->shutdown(*this));
            noteGpuTeardownStageBestEffort_("platform");
        }

        return first_err;
    }

    void Application::resetGpuTeardownEpoch_() noexcept {
        // Allocation-free by construction: Array::clear releases without
        // allocating; error_code/disposition/flag assignment cannot throw.
        m_gpu_teardown_prepared = false;
        m_gpu_teardown_disposition = GpuQuiescence::kUnknown;
        m_gpu_teardown_error = default_value_v;
        m_gpu_teardown_stages.clear();
        m_wait_pump_error = default_value_v;
        m_display_transactions_stopped = false;
        m_display_controller.startTransactions();
        bindDisplaySurface_({});
        // Fresh settlement epoch: stale lane results from a finished run
        // can never match (and so never release) a record again. Any
        // unsettled reservation records from the previous epoch are gone —
        // the teardown settle discarded them under a proven disposition.
        ++m_reservation_epoch;
    }

    void Application::noteTeardownStage_(const char *stage) noexcept {
        noteGpuTeardownStageBestEffort_(stage);
    }

    void Application::noteGpuTeardownStageBestEffort_(const char *stage) noexcept {
        try {
            m_gpu_teardown_stages.push_back(std::string{stage});
        } catch (...) {
            // Diagnostics are best-effort: an allocation failure here must
            // neither mask the recorded disposition nor escape toward
            // ordinary destruction on the UNKNOWN path.
        }
    }

    std::error_code Application::prepareGpuTeardown_() {
        // B1'b fail-closed: no exception escapes this function before the
        // caller fail-stops on UNKNOWN. Every allocating/best-effort step
        // below is guarded; completion flags publish only after
        // classification; the prepared flag sets last.
        if (m_gpu_teardown_prepared) {
            return m_gpu_teardown_error;
        }

        m_display_transactions_stopped = true;
        m_display_controller.stopTransactions();
        if (m_display_controller.transactionState() == EDisplayTransaction::degraded and not m_wait_pump_error) {
            m_wait_pump_error = m_display_controller.transactionError();
        }

        // B3' entry hook (tests only): owners alive, nothing released yet.
        // Best-effort instrumentation — a throwing hook must not bypass
        // proof: fall through to classification below.
        try {
            if (m_gpu_teardown_entry_hook) {
                std::move(m_gpu_teardown_entry_hook)();
            }
        } catch (...) {
            // Instrumentation cannot bypass classification or the UNKNOWN fail-stop boundary.
        }

        // A native apply can precede a full control queue. It is not a cancellable
        // no-op: finish its restoration before quarantining the renderer. Existing
        // owner-lane handoffs drain accepted controls without admitting a frame.
        if (m_pending_window_change and m_window_operation_transaction != 0u) {
            try {
                for (u32 attempt = 0u; attempt < 3u and m_display_controller.pending(); ++attempt) {
                    m_display_controller.cancelUnsubmitted();
                    const std::error_code transaction_error = serviceDisplayTransaction_();
                    if (transaction_error and not m_wait_pump_error) {
                        m_wait_pump_error = transaction_error;
                    }
                    if (renderLaneOwnsRenderer_()) {
                        bool acknowledged = false;
                        const std::error_code drain_error = m_render_lane->runOnLaneSync(
                            []() -> std::error_code { return {}; }, &acknowledged);
                        if (drain_error or not acknowledged) {
                            if (not m_wait_pump_error) {
                                m_wait_pump_error = drain_error ? drain_error
                                                                : std::make_error_code(std::errc::timed_out);
                            }
                            break;
                        }
                    }
                    if (const std::error_code error = pumpRenderCompletions_(); error and not m_wait_pump_error) {
                        m_wait_pump_error = error;
                    }
                }
                if (m_display_controller.pending() and not m_wait_pump_error) {
                    m_wait_pump_error = std::make_error_code(std::errc::state_not_recoverable);
                }
            } catch (...) {
                if (not m_wait_pump_error) {
                    m_wait_pump_error = std::make_error_code(std::errc::state_not_recoverable);
                }
            }
        }

        // Transactions are now settled; stop backend admission before GPU
        // completion classification. Inline access has no concurrent submitter.
        if (m_renderer and not renderLaneOwnsRenderer_()) {
            m_renderer->quarantineAdmission();
        }

        if (not m_renderer) {
            // Trivially idle: no GPU resources exist in this epoch.
            m_gpu_teardown_disposition = GpuQuiescence::kIdleProven;
            m_gpu_teardown_error = default_value_v;
            m_gpu_teardown_prepared = true;
            noteGpuTeardownStageBestEffort_("quiescence");
            noteGpuTeardownStageBestEffort_("idle-proven");
            return default_value_v;
        }

        // Quiescence is established WITHOUT releasing anything; release
        // happens in the shutdown entries above/below, only after a safe
        // disposition. Never signal the app fence here. On the lane the
        // whole classification (quarantine + wait) runs there, so the
        // ledger/fence are never touched from two threads at once.
        GpuQuiescenceResult quiescence{};
        if (renderLaneOwnsRenderer_()) {
            // Fail-closed: a throwing sync primitive proves nothing either.
            std::error_code sync_err{};
            // Acknowledged completion is the ONLY permission to read the
            // operation-published outcome: on timeout the lane may still
            // execute the posted operation, so the outcome must never be
            // read (and never lived on a panicking stack) — it travels in
            // heap-owned storage the posted operation itself retains.
            bool acknowledged = false;
            struct QuiescenceHandoffState final {
                GpuQuiescenceResult m_quiescence{};
            };
            // Exception boundary: the allocation lives INSIDE the try so a
            // make_shared failure fail-stops through the same UNKNOWN path
            // as any other handoff failure (never past it with no proof).
            std::shared_ptr<QuiescenceHandoffState> handoff_state{};
            try {
                handoff_state = std::make_shared<QuiescenceHandoffState>();
                sync_err = m_render_lane->runOnLaneSync(
                    [this, handoff_state]() -> std::error_code {
                        PPR_ASSERT(m_renderer != nullptr);
                        m_renderer->quarantineAdmission();
                        handoff_state->m_quiescence = m_renderer->establishGpuQuiescence();
                        return handoff_state->m_quiescence.m_error;
                    },
                    &acknowledged);
            } catch (...) {
                sync_err = std::make_error_code(std::errc::state_not_recoverable);
            }
            if (acknowledged) {
                // The lane finished the operation and published the result
                // before the handoff acknowledged: the outcome is stable.
                // A classified outcome (e.g. kRemovalConfirmed with its own
                // error) is preserved, never downgraded to UNKNOWN; the
                // sync-level error is retained separately on the result.
                quiescence = handoff_state->m_quiescence;
                quiescence.m_error = quiescence.m_error ? quiescence.m_error : sync_err;
            } else if (sync_err == std::make_error_code(std::errc::timed_out)) {
                // Fail-stop BEFORE borrowed storage could expire: the
                // timed-out operation may still run concurrently, and an
                // atomic flag (the old lane_ran) cannot prove it didn't.
                // The heap-owned handoff state outlives this frame.
                failStopOnUnknownGpuTeardown_(sync_err);
            } else {
                // The lane never took the operation: nothing further can
                // be proven; the disposition is UNKNOWN.
                quiescence = GpuQuiescenceResult{
                    .m_disposition = GpuQuiescence::kUnknown,
                    .m_error = sync_err,
                };
            }
        } else {
            try {
                quiescence = m_renderer->establishGpuQuiescence();
            } catch (const std::bad_alloc &) {
                quiescence = GpuQuiescenceResult{
                    .m_disposition = GpuQuiescence::kUnknown,
                    .m_error = std::make_error_code(std::errc::not_enough_memory),
                };
            } catch (...) {
                quiescence = GpuQuiescenceResult{
                    .m_disposition = GpuQuiescence::kUnknown,
                    .m_error = std::make_error_code(std::errc::state_not_recoverable),
                };
            }
        }
        // Publish completion flags ONLY after classification above.
        m_gpu_teardown_disposition = quiescence.m_disposition;
        m_gpu_teardown_error = quiescence.m_error;
        m_gpu_teardown_prepared = true;
        // Diagnostics last and best-effort: on UNKNOWN an allocation
        // failure here can no longer gate the caller's fail-stop.
        noteGpuTeardownStageBestEffort_("quiescence");
        switch (quiescence.m_disposition) {
            case GpuQuiescence::kIdleProven: noteGpuTeardownStageBestEffort_("idle-proven"); break;
            case GpuQuiescence::kRemovalConfirmed: noteGpuTeardownStageBestEffort_("removal-confirmed"); break;
            case GpuQuiescence::kUnknown: noteGpuTeardownStageBestEffort_("unknown"); break;
        }
        return m_gpu_teardown_error;
    }

    void Application::failStopOnUnknownGpuTeardown_(const std::error_code quiescence_error) noexcept {
        // PIX breadcrumb only: no logging, no allocation — the process exits below.
        const hal::ProfileScope scope_frame_fail_stop{"PPR.Frame.FailStop"};
        // Best-effort diagnostics first: no logger-drain dependency — the
        // process is exiting and the OS reclaims retained GPU objects.
        try {
            if (m_on_gpu_teardown_failure != nullptr) {
                m_on_gpu_teardown_failure(quiescence_error);
            }
        } catch (...) {
            // A throwing handler must not unwind through teardown: fall
            // through to the unconditional termination below.
        }
        std::fflush(nullptr);
        std::_Exit(kUnknownGpuCompletionFatalExit);
    }

    std::error_code Application::update(TimeSpan dt) {
        const hal::ProfileScope scope{"App.Update"};
        if (not m_platform) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->update(dt));
        return default_value_v;
    }

    std::error_code Application::render() {
        const hal::ProfileScope scope{"App.Render"};
        if (m_domain.m_needs_rendering and not m_renderer) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        return default_value_v;
    }
}
