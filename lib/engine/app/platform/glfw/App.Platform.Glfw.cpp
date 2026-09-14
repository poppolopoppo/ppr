module;
#include "pP/Macros.h"
#include "App.Platform.Glfw.include.hpp"

module engine.app;

import :platform.glfw;
import :platform.glfw.input;
import :platform.glfw.player;
import :platform.glfw.window;
import std;
import engine.core;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(GlfwPlatform, info, none)

    namespace platform {
        class GlfwErrorCategory : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override { return "glfw"; }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<errc>(ev)) {
                    case errc::ok: return "indicates success";
                    case errc::fail: return "generic failure code - meaning a serious error occurred and the call couldn't complete";
                    case errc::initialization_failed: return "indicates that the plaform failed to initialize";
                    case errc::invalid_argument: return "indicates that an argument passed in as parameter to a method is invalid";

                    default: return std::format("unknown glfw result ({})", ev);
                }
            }

            [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
                switch (static_cast<errc>(ev)) {
                    case errc::invalid_argument: return std::errc::invalid_argument;
                    default: return {ev, *this};
                }
            }
        };

        static constexpr GlfwErrorCategory g_glfw_error_category{};

        [[nodiscard]] const std::error_category &error_category() noexcept {
            return g_glfw_error_category;
        }

        [[nodiscard]] std::error_code make_error_code(const int result) noexcept {
            return result == GL_TRUE ? std::error_code{} : std::error_code{static_cast<int>(errc::fail), g_glfw_error_category};
        }

        [[nodiscard]] std::error_code make_error_code(const errc error_code) noexcept {
            return make_error_code(enumOrd(error_code));
        }
    }

    std::unique_ptr<IPlatform> IPlatform::create() noexcept {
        return std::make_unique<GlfwPlatform>();
    }

    GlfwPlatform::GlfwPlatform() noexcept = default;

    safe_ptr<Application> GlfwPlatform::getApplication() const noexcept {
        return m_application;
    }

    safe_ptr<IInputService> GlfwPlatform::getInputService() const noexcept {
        return safe_ptr(m_input_service.get());
    }

    safe_ptr<IWindowService> GlfwPlatform::getWindowService() const noexcept {
        return safe_ptr(m_window_service.get());
    }

    safe_ptr<IPlayerService> GlfwPlatform::getPlayerService() const noexcept {
        return safe_ptr(m_player_service.get());
    }

    std::string_view GlfwPlatform::getPlatformName() const noexcept { return "GLFW"; }

    platform::Version GlfwPlatform::getPlatformVersion() const noexcept {
        int major = 0, minor = 0, revision = 0;
        ::glfwGetVersion(&major, &minor, &revision);
        return {.m_major = major, .m_minor = minor, .m_revision = revision};
    }

    std::error_code GlfwPlatform::initialize(Application &app) {
        PPR_LOG(GlfwPlatform, info, "initialize GLFW platform", {
            {"version", ::glfwGetVersionString()}
        });

        PPR_ASSERT(not m_application.isValid());
        m_application.reset(&app);

        ::glfwSetErrorCallback([](int error_code, const char *description) {
            PPR_LOG(GlfwPlatform, error, "GLFW error", {{"error_code", error_code}, {"description", description}});
        });

        constexpr ::GLFWallocator glfw_allocator{
            .allocate = [](size_t size, void *) -> void * { return std::malloc(size); },
            .reallocate = [](void *block, size_t size, void *) -> void * { return std::realloc(block, size); },
            .deallocate = [](void *block, void *) { std::free(block); },
            .user = nullptr,
        };
        // Intentional process-lifetime state: GLFW offers no API to clear a
        // custom allocator, so these hooks stay installed on every exit path.
        ::glfwInitAllocator(&glfw_allocator);

        if (not::glfwInit()) [[unlikely]] {
            PPR_LOG(GlfwPlatform, error, "failed to init GLFW");
            ::glfwSetErrorCallback(nullptr);
            return platform::errc::initialization_failed;
        }

        // Rolls back every stage committed so far, then always releases GLFW
        // itself; returns the stage error that triggered the unwind.
        bool success = false;
        PPR_DEFER {
            if (not success) [[unlikely]] {
                std::ignore = shutdown(app);
            }
        };

        const ApplicationDomain domain = app.getDomain();
        ServicesStore &app_services = app.getServices();

        if (not domain.m_is_headless) {
            m_window_service = std::make_unique<GlfwWindow>();
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, m_window_service->initialize());
            app_services.insert_or_assign<IWindowService>(m_window_service.get());
        }

        if (domain.m_is_interactive) {
            m_input_service = std::make_unique<GlfwInput>();
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, m_input_service->initialize());
            app_services.insert_or_assign<IInputService>(m_input_service.get());
        }

        if (domain.m_needs_presence) {
            m_player_service = std::make_unique<GlfwPlayer>();
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, m_player_service->initialize(*m_input_service));
            app_services.insert_or_assign<IPlayerService>(m_player_service.get());
        }

        success = true;
        return default_value_v;
    }

    std::error_code GlfwPlatform::shutdown(Application &app) {
        PPR_LOG(GlfwPlatform, info, "shutdown GLFW platform", {
            {"version", ::glfwGetVersionString()}
        });

        // Idempotent: Application::shutdown() calls here on every teardown,
        // and initialize() already shuts down on failure, so a second call
        // must not re-fire the application-identity assert below.
        if (not m_application.isValid()) {
            return default_value_v;
        }
        PPR_ASSERT(m_application == &app);
        PPR_DEFER {
            m_application.reset();
        };

        ServicesStore &app_services = app.getServices();

        std::error_code first_err{};
        if (m_player_service) {
            PPR_VERIFY(app_services.erase<IPlayerService>(*m_player_service));
            PPR_RETAIN_ERROR_ON_FAIL(GlfwPlatform, first_err, m_player_service->shutdown());
            m_player_service.reset();
        }

        if (m_input_service) {
            PPR_VERIFY(app_services.erase<IInputService>(*m_input_service));
            PPR_RETAIN_ERROR_ON_FAIL(GlfwPlatform, first_err, m_input_service->shutdown());
            m_input_service.reset();
        }

        if (m_window_service) {
            PPR_VERIFY(app_services.erase<IWindowService>(*m_window_service));
            PPR_RETAIN_ERROR_ON_FAIL(GlfwPlatform, first_err, m_window_service->shutdown());
            m_window_service.reset();
        }

        ::glfwTerminate();
        ::glfwSetErrorCallback(nullptr);
        return default_value_v;
    }

    std::error_code GlfwPlatform::update(const TimeSpan dt) {
        // Clear transient input state BEFORE window-event dispatch appends this
        // frame's characters. Safe to run first: device polling is event-pump
        // independent (gamepads poll via glfwGetGamepadState, keyboard/mouse polls
        // only reset transient
        // state), and dispatch below then uses this frame's delta time.
        if (GlfwInput *const p_inputs = m_input_service.get()) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, p_inputs->pollInputDevices(dt));
        }

        if (GlfwWindow *const p_windows = m_window_service.get()) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, p_windows->pollEvents());
        }

        return default_value_v;
    }
}
