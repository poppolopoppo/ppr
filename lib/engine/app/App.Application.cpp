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
        : m_application_clock(ITimerClock::steady()), m_platform(IPlatform::create()),
          m_arguments(argv.begin(), argv.end()), m_name(name), m_domain(std::move(domain)) {
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

        if (const std::error_code init_err = initialize()) {
            completed = true;
            return finishRun_(init_err);
        }

        PPR_LOG(App, emphasis, "🏁 run application loop");

        // Run-loop invariant (A1 lesson): terminate on stable state — the
        // graceful-exit latch or the latched cancel error — never on the
        // consumable event pulse alone. run() reports the same stable error.
        while (not isExitRequested()) {
            const hal::ProfileScope scope{"App.Frame"};
            std::error_code first_err{};

            // Stage: advance clock (wakeable pacer wait, then zero-target
            // tick). An exit latched during the wait stops admission here:
            // the frame retires without client work and the loop observes
            // termination at the top.
            TimeSpan dt{};
            {
                const hal::ProfileScope scope_clock{"App.Frame.Clock"};
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, advanceClock_(&dt));
            }

            if (isExitRequested()) {
                // Clock-error + exit coincidence: retain the clock error for
                // run()'s return value, and retire the frame without client
                // or render work on this path.
                if (first_err) {
                    m_request_exit(first_err);
                }
                break;
            }

            try {
                {
                    const hal::ProfileScope scope_update{"App.Frame.Update"};
                    PPR_RETAIN_ERROR_ON_FAIL(App, first_err, update(dt));
                }

                {
                    const hal::ProfileScope scope_render{"App.Frame.Render"};
                    PPR_RETAIN_ERROR_ON_FAIL(App, first_err, render());
                }

            } catch (const std::system_error &e) {
                m_request_exit(e.code());
            } catch (const std::invalid_argument &) {
                m_request_exit(std::make_error_code(std::errc::invalid_argument));
            } catch (const std::bad_alloc &) {
                m_request_exit(std::make_error_code(std::errc::not_enough_memory));
            } catch (...) {
                m_request_exit(std::make_error_code(std::errc::state_not_recoverable));
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
        completed = true;
        return finishRun_(loop_err);
    }

    std::error_code Application::advanceClock_(TimeSpan *out_delta_time) {
        // P0 serial-explicit pacing: wakeable external waits against the
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
        PPR_LOG(App, info, "shut down application", {{"name", m_name}, {"platform", hal::platformName()}});

        PPR_DEFER {
            context::setApplication(SharedContext{});

            m_lifecycle.reset();
            m_request_exit.reset();
        };

        std::error_code first_err{};

        if (m_renderer) {
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_renderer->shutdown());
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
        }

        return first_err;
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
