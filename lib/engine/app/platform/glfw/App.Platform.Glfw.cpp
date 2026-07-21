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

    /*static*/
    SharedPlatform IPlatform::get() noexcept {
        static GlfwPlatform g_instance{};
        return SharedPlatform(&g_instance);
    }

    safe_ptr<IInputService> GlfwPlatform::getInputService() const noexcept {
        return safe_ptr<IInputService>{&GlfwInput::get()};
    }

    safe_ptr<IWindowService> GlfwPlatform::getWindowService() const noexcept {
        return safe_ptr<IWindowService>{m_window_service};
    }

    safe_ptr<IPlayerService> GlfwPlatform::getPlayerService() const noexcept {
        return GlfwPlayer::get();
    }

    std::string_view GlfwPlatform::getPlatformName() const noexcept { return "GLFW"; }

    platform::Version GlfwPlatform::getPlatformVersion() const noexcept {
        int major = 0, minor = 0, revision = 0;
        ::glfwGetVersion(&major, &minor, &revision);
        return {.m_major = major, .m_minor = minor, .m_revision = revision};
    }

    std::error_code GlfwPlatform::initialize(Application &application) {
        ::glfwSetErrorCallback([](int error_code, const char *description) {
            PPR_LOG(GlfwPlatform, error, "GLFW error", {{"error_code", error_code}, {"description", description}});
        });

        constexpr ::GLFWallocator glfw_allocator{
            .allocate = [](size_t size, void *) -> void * { return std::malloc(size); },
            .reallocate = [](void *block, size_t size, void *) -> void * { return std::realloc(block, size); },
            .deallocate = [](void *block, void *) { std::free(block); },
            .user = nullptr,
        };
        ::glfwInitAllocator(&glfw_allocator);

        if (not::glfwInit()) [[unlikely]] {
            PPR_LOG(GlfwPlatform, error, "failed to init GLFW");
            return platform::errc::initialization_failed;
        }
        m_glfw_initialized = true;

        m_window_service = safe_ptr(&GlfwWindow::get());

        m_window_service->initialize();

        const safe_ptr input_service{&GlfwInput::get()};
        PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, input_service->initialize());

        m_window_service->setInputService(input_service);

        ServicesStore &app_services = application.getServices();
        std::ignore = app_services.insert(safe_ptr<IInputService>{input_service});
        std::ignore = app_services.insert(safe_ptr<IWindowService>{m_window_service});
        std::ignore = app_services.insert(safe_ptr<IPlayerService>{GlfwPlayer::get()});

        return default_value_v;
    }

    std::error_code GlfwPlatform::shutdown(Application &application) {
        if (not m_glfw_initialized) [[unlikely]] {
            return default_value_v;
        }

        PPR_ASSERT(m_window_service.isValid());
        if (m_window_service) [[likely]] {
            m_window_service->setInputService({});
            m_window_service->shutdown();
            m_window_service = nullptr;
        }

        ServicesStore &app_services = application.getServices();
        app_services.erase<IInputService>();
        app_services.erase<IPlayerService>();
        app_services.erase<IWindowService>();

        PPR_RETURN_ERROR_ON_FAIL(GlfwPlatform, GlfwInput::get().shutdown());

        ::glfwTerminate();
        m_glfw_initialized = false;

        return default_value_v;
    }
}
