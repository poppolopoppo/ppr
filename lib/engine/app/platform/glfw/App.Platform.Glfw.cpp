module;
#include <GLFW/glfw3.h>
#include "pP/Macros.h"

module engine.app;

import :platform.glfw;
import :platform.glfw.input;
import :platform.glfw.window;
import engine.core;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(GlfwPlatform, info, none)

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
        return safe_ptr<IPlayerService>{&GlfwInput::get()};
    }

    std::string_view GlfwPlatform::getPlatformName() const noexcept { return "GLFW"; }

    PlatformVersion GlfwPlatform::getPlatformVersion() const noexcept {
        int major = 0, minor = 0, revision = 0;
        ::glfwGetVersion(&major, &minor, &revision);
        return PlatformVersion{.m_major = major, .m_minor = minor, .m_revision = revision};
    }

    void GlfwPlatform::initializePlatform(Application &application) {
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
            return;
        }
        m_glfw_initialized = true;

        m_window_service = &GlfwWindow::get();

        m_window_service->initialize();
        GlfwInput::get().initialize();

        m_window_service->setInputService(safe_ptr<GlfwInput>{&GlfwInput::get()});

        application.getServices().insert(safe_ptr<IInputService>{&GlfwInput::get()});
        application.getServices().insert(safe_ptr<IWindowService>{m_window_service});
        application.getServices().insert(safe_ptr<IPlayerService>{&GlfwInput::get()});
    }

    void GlfwPlatform::shutdownPlatform(Application &application) {
        if (m_window_service) [[likely]] {
            m_window_service->setInputService({});
            m_window_service->shutdown();
            m_window_service = nullptr;
        }
        application.getServices().erase<IInputService>();
        application.getServices().erase<IPlayerService>();
        application.getServices().erase<IWindowService>();
        GlfwInput::get().shutdown();
        if (m_glfw_initialized) {
            ::glfwTerminate();
        }
    }
}
