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
        : m_platform(IPlatform::create()),
          m_arguments(argv.begin(), argv.end()),
          m_name(name),
          m_domain(std::move(domain)) {
        PPR_ASSERT(m_platform != nullptr);

        hal::disableSystemErrorReporting();
        hal::installDebugAssertHooks();
    }

    Application::~Application() noexcept = default;

    void Application::requestApplicationExit(const std::error_code clause) noexcept {
        if (PPR_ENSURE(m_request_exit.isValid())) [[likely]] {
            m_request_exit(clause);
        }
    }

    void Application::setTargetFrameDuration(TimeDuration frame_time) noexcept {
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
        if (m_has_torn_down) [[unlikely]] {
            PPR_RETURN_ERROR_ON_FAIL(App, std::errc::operation_not_permitted);
        }

        PPR_RETURN_ERROR_ON_FAIL(App, initialize());

        std::error_code first_err{};
        PPR_DEFER {
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, shutdown());
        };

        while (not m_lifecycle->error()) {
            m_application_clock.tick(time::now());

            // throttle if necessary to target desired frame rate, if any provided
            if (m_application_clock.m_elapsed < m_target_frame_duration.value_or(zero_v)) {
                std::this_thread::sleep_for(m_target_frame_duration.value() - m_application_clock.m_elapsed);

                m_application_clock.m_now = m_application_clock.last();
                m_application_clock.tick(time::now());
            }

            try {
                PPR_RETAIN_ERROR_ON_FAIL(App, first_err, update(m_application_clock.m_elapsed));
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
        }

        PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_lifecycle->error());
        return first_err;
    }

    std::error_code Application::initialize() {
        if (m_has_torn_down) [[unlikely]] {
            PPR_RETURN_ERROR_ON_FAIL(App, std::errc::operation_not_permitted);
        }

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

        m_application_clock.reset(time::now());

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
        // Latch first: every teardown attempt (including rollback and
        // best-effort paths that retain an error below) consumes the instance.
        // shutdown() itself stays idempotent-success; only reuse is rejected.
        m_has_torn_down = true;

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

        // Teardown inverse of setup: platform initialized first, so it shuts
        // down last, after graphics. Best effort: retain the first error and
        // always attempt the platform pair it committed (safe on partial init:
        // GlfwPlatform::shutdown is idempotent over unowned services).
        if (m_platform) {
            PPR_RETAIN_ERROR_ON_FAIL(App, first_err, m_platform->shutdown(*this));
        }

        return first_err;
    }

    std::error_code Application::update(TimeSpan dt) {
        // Pump deadline callbacks so withDeadline/withTimeout contexts can fire.
        TimerManager::mainTimer().tick();

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->update(dt));
        return default_value_v;
    }

    std::error_code Application::render() {
        return default_value_v;
    }
}
