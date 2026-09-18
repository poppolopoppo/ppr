module;

#include "pP/Macros.h"

module engine.app;

import :application;
import :platform;
import :renderer;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(App, debug, none)

    Application::Application(ApplicationDomain domain, const std::string_view name, const std::span<const char *const> argv)
        : m_application_clock(ITimerClock::steady()),
          m_platform(IPlatform::create()),
          m_arguments(argv.begin(), argv.end()),
          m_name(name),
          m_domain(std::move(domain)) {
        PPR_ASSERT(m_platform != nullptr);

        hal::disableSystemErrorReporting();
        hal::installDebugAssertHooks();
    }

    Application::~Application() noexcept = default;

    std::optional<TimeDuration> Application::getTargetFrameDuration() const noexcept {
        if (m_has_background_priority) {
            constexpr TimeDuration background_frame_duration{1.0 / 5.0};
            return background_frame_duration; // 5fps
        }
        return m_target_frame_duration;
    }

    void Application::requestExit(const std::error_code clause) const noexcept {
        if (PPR_ENSURE(m_request_exit.isValid())) [[likely]] {
            PPR_LOG(App, info, "request exit", {
                {"category", clause.category().name()},
                {"cause", clause.message()}
                });

            m_request_exit(clause);
        }
    }

    void Application::setBackgroundPriority(const bool throttle) noexcept {
        m_has_background_priority = throttle;
    }

    void Application::setTargetFrameDuration(const TimeDuration frame_time) noexcept {
        m_target_frame_duration = frame_time;
    }

    void Application::setTargetFrameDuration(std::nullopt_t) noexcept {
        m_target_frame_duration.reset();
    }

    void Application::setTargetFrameRate(const int fps) noexcept {
        if (fps > 0) {
            setTargetFrameDuration(TimeDuration{1.0 / static_cast<double>(fps)});
        } else {
            setTargetFrameDuration(std::nullopt);
        }
    }

    std::error_code Application::run() {
        PPR_DEFER {
            std::error_code shutdown_err{};
            PPR_RETAIN_ERROR_ON_FAIL(App, shutdown_err, shutdown());

            if (shutdown_err) [[unlikely]] {
                m_request_exit(shutdown_err);
            }
        };

        PPR_RETURN_ERROR_ON_FAIL(App, initialize());

        PPR_LOG(App, emphasis, "🏁 run application loop");

        while (not m_lifecycle->pollEvent()) {
            std::error_code first_err{};

            // throttle if necessary to target desired frame rate, if any provided
            TimeSpan dt{};
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_application_clock.tick(&dt,
                getTargetFrameDuration().value_or({})));

            try {
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, update(dt));

                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, render());
            } catch (const std::system_error &e) {
                m_request_exit(e.code());
            } catch (const std::invalid_argument &) {
                m_request_exit(std::make_error_code(std::errc::invalid_argument));
            } catch (const std::bad_alloc &) {
                m_request_exit(std::make_error_code(std::errc::not_enough_memory));
            } catch (...) {
                m_request_exit(std::make_error_code(std::errc::state_not_recoverable));
            }

            Log::flush();

            if (first_err) [[unlikely]] {
                m_request_exit(first_err);
            }
        }

        PPR_LOG(App, emphasis, "stop application loop, bye 👋");

        return m_lifecycle->error();
    }

    std::error_code Application::initialize() {
        m_application_clock.reset();

        PPR_LOG(App, info, "starting application", {
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

        if (m_domain.m_needs_rendering) {
            const safe_ptr<IShaderService> shader_service = IShaderService::get();
            PPR_RETURN_ERROR_ON_FAIL(App, shader_service->initialize());
            m_services.insert_or_assign(shader_service);

            const safe_ptr<IRhiService> rhi_service = IRhiService::get();
            PPR_RETURN_ERROR_ON_FAIL(App, rhi_service->initialize(rhi::DeviceType::Default, *shader_service));
            m_services.insert_or_assign(rhi_service);

            auto renderer = std::make_unique<Renderer>();
            PPR_RETURN_ERROR_ON_FAIL(App, renderer->initialize(*rhi_service));
            m_renderer = std::move(renderer);
        }

        success = true;
        return default_value_v;
    }

    std::error_code Application::shutdown() {
        PPR_LOG(App, info, "shut down application", {
            {"name", m_name},
            {"platform", hal::platformName()}
            });

        std::error_code first_err{};

        if (m_renderer) {
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_renderer->shutdown());
            m_renderer.reset();
        }

        if (m_domain.m_needs_rendering) {
            const safe_ptr<IRhiService> rhi_service = IRhiService::get();
            if (m_services.erase(*rhi_service)) {
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, rhi_service->shutdown());
            }

            const safe_ptr<IShaderService> shader_service = IShaderService::get();
            if (m_services.erase(*shader_service)) {
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, shader_service->shutdown());
            }
        }

        if (m_platform) {
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_platform->shutdown(*this));
        }

        return first_err;
    }

    std::error_code Application::update(TimeSpan dt) {
        if (not m_platform) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->update(dt));
        return default_value_v;
    }

    std::error_code Application::render() {
        if (m_domain.m_needs_rendering and not m_renderer) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        return default_value_v;
    }
}
