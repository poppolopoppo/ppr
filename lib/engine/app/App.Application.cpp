module;

#include "pP/Macros.h"

module engine.app;

import :application;
import :platform;
import :renderer;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(App, debug, none)

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
        if (m_has_background_priority) {
            constexpr TimeSpan background_frame_duration = std::chrono::duration_cast<TimeSpan>(
                TimeDuration{1.0 / 5.0});
            return background_frame_duration; // 5fps
        }
        return m_target_frame_time;
    }

    void Application::requestExit(const std::error_code clause) const noexcept {
        if (PPR_ENSURE(m_request_exit.isValid())) [[likely]] {
            PPR_LOG(App, info, "request exit", {{"category", clause.category().name()}, {"cause", clause.message()}});

            if (not clause) {
                // Graceful exit is not cancellation: raise the exit latch so
                // the run loop terminates, but record no error — run() still
                // returns success.
                m_exit_requested.store(true, std::memory_order_release);
                return;
            }
            m_request_exit(clause);
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

            // Stage: advance clock (wakeable pacer wait, then zero-target
            // tick). A clock error OR a latched exit retires the frame here:
            // the error is latched for run()'s return value and the loop
            // leaves WITHOUT client or render work on this path.
            TimeSpan dt{};
            {
                const hal::ProfileScope scope_clock{"App.Frame.Clock"};
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, advanceClock_(&dt));
            }

            if (first_err or isExitRequested()) {
                if (first_err) {
                    m_request_exit(first_err);
                }
                break;
            }

            // Admission + client stages share one exception boundary; every
            // handler retains the FIRST error, never overwriting it.
            try {
                // B2' admission gate BEFORE the whole virtual update stage
                // (derived pre-work included): retire + probe, then branch.
                // Terminal (quarantine included) retains first_err, latches
                // it, and leaves BEFORE carry/update/render. Capacity-busy
                // accumulates dt into the single Application-owned carry,
                // runs PLATFORM COLLECTION ONLY (never update(dt): device
                // time must not advance on a skipped frame), latches a
                // collection failure, and finishes the iteration.
                bool admitted_skip = false;
                if (const std::error_code probe_err = probeFrameAdmissionPreUpdate_(admitted_skip)) {
                    if (not first_err) {
                        first_err = probe_err;
                    }
                    m_request_exit(first_err);
                    break;
                }
                if (admitted_skip) {
                    noteSkippedFrameDt(dt);
                    if (m_platform) {
                        PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_platform->pumpNativeEvents());
                    }
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

            if (first_err) [[unlikely]] {
                m_request_exit(first_err);
            }

            ++m_frame_index;
        }

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

    std::error_code Application::probeFrameAdmissionPreUpdate_(bool &out_skip) {
        out_skip = false;
        if (not m_domain.m_needs_rendering or not m_renderer) {
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
        return default_value_v;
    }

    std::error_code Application::advanceClock_(
        TimeSpan *out_delta_time) { // P0 serial-explicit pacing: wakeable external waits against the
        // previous-tick-anchored deadline (min-period policy, no catch-up),
        // then the manager ticks with a zero target so now() stays latched
        // and only callback dispatch + dt accounting run. Focus/background/
        // uncapped transitions re-evaluate here every frame via
        // getTargetFrameTime(). Each wait slices at the wake quantum and the
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

            auto renderer = std::make_unique<Renderer>();
            {
                const hal::ProfileScope scope_renderer{"App.Initialize.Renderer"};
                PPR_RETURN_ERROR_ON_FAIL(App, renderer->initialize(*rhi_service));
            }
            m_renderer = std::move(renderer);
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
        // disposition below authorizes release.
        if (const std::error_code teardown_err = prepareGpuTeardown_()) {
            if (not first_err) {
                first_err = teardown_err;
            }
        }
        if (m_gpu_teardown_disposition == GpuQuiescence::kUnknown) {
            failStopOnUnknownGpuTeardown_(m_gpu_teardown_error);
        }

        // Safe paths only from here: quiescence is proven idle or removal
        // is confirmed, so entry/release diagnostics may log again.
        PPR_LOG(App, info, "shut down application", {{"name", m_name}, {"platform", hal::platformName()}});

        if (m_renderer) {
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_renderer->shutdown());
            m_gpu_teardown_stages.push_back("renderer");
            m_renderer.reset();
        }

        if (m_domain.m_needs_rendering) {
            const safe_ptr<IRhiService> rhi_service = IRhiService::get();
            if (m_services.tryGet<IRhiService>() == rhi_service) {
                const std::error_code shutdown_error = rhi_service->shutdown();
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, shutdown_error);
                if (not shutdown_error) {
                    PPR_VERIFY(m_services.erase(*rhi_service));
                }
            }
            m_gpu_teardown_stages.push_back("rhi");

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
            m_gpu_teardown_stages.push_back("platform");
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

        // Stop admission permanently FIRST: no new frame may be built while
        // GPU state of unknown completion is being classified. noexcept.
        if (m_renderer) {
            m_renderer->quarantineAdmission();
        }

        // B3' entry hook (tests only): owners alive, nothing released yet.
        // Best-effort instrumentation — a throwing hook must not bypass
        // proof: fall through to classification below.
        try {
            if (m_gpu_teardown_entry_hook) {
                std::move(m_gpu_teardown_entry_hook)();
            }
        } catch (...) {
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
        // disposition. Never signal the app fence here.
        GpuQuiescenceResult quiescence{};
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
