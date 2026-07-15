module;

#include <GLFW/glfw3.h>

#include "pP/Macros.h"

module engine.app;

import std;
import engine.core;
import engine.rhi;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(App, debug, none)

    Application::Application(const std::string_view name, const std::span<const char *const> argv)
        : m_arguments(argv.begin(), argv.end()),
          m_name(name) {
    }

    Application::~Application() noexcept = default;

    void Application::setExitCode(const int exitCode) noexcept {
        m_exitCode = exitCode;
    }

    int Application::run() {
        setExitCode(exit_no_error);

        if (initialize()) [[likely]] {
            PPR_DEFER {
                terminate();
            };

            try {
                while (update()) [[likely]] {
                    render();
                }
            } catch (const std::exception &e) {
                setExitCode(exit_exception);
                PPR_LOG(App, error, "caught exception inside the main loop", {{"what", e.what()}});
                throw;
            }
        }

        return m_exitCode;
    }

    bool Application::initialize() {
        PPR_LOG(App, info, "starting application", {
            {"name", m_name},
            {"platform", hal::platformName()},
            {"args", [this]() noexcept -> opaque::TransformView {
                return opaque::TransformView(m_arguments);
            }},
        });

        // set GLFW error callback to write to the logger
        ::glfwSetErrorCallback([](const int error_code, const char *const description) {
            PPR_LOG(App, error, "caught GLFW error", {{"error_code", error_code}, {"description", description}});
        });

        // set GLFW allocator explicitly
        constexpr ::GLFWallocator glfw_allocator{
            .allocate = [](const std::size_t size, [[maybe_unused]] void *user) -> void * { return std::malloc(size); },
            .reallocate = [](void *const block, const std::size_t size, [[maybe_unused]] void *user) -> void * { return std::realloc(block, size); },
            .deallocate = [](void *const block, [[maybe_unused]] void *user) -> void { std::free(block); },
            .user = nullptr,
        };
        ::glfwInitAllocator(&glfw_allocator);

        // actual initialization of GLFW
        if (not ::glfwInit()) [[unlikely]] {
            setExitCode(exit_failed_init);
            PPR_LOG(App, error, "failed to init GLFW");
            return false;
        }
        PPR_LOG(App, info, "successfully initialized GLFW",
            {
                {"version", ::glfwGetVersionString()},
            });

        // rhi initialization
        if (not rhi::initialize()) [[unlikely]] {
            setExitCode(exit_failed_init);
            PPR_LOG(App, error, "failed to init RHI");
            return false;
        }
        PPR_LOG(App, info, "successfully initialized RHI");

        return true;
    }

    bool Application::update() {
        if (::glfwWindowShouldClose(nullptr)) [[unlikely]] {
            PPR_LOG(App, error, "main GLFW window should close");
            return false;
        }

        ::glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        const TimeSpan dt = now - m_last_frame_time;
        m_last_frame_time = now;

        if (const safe_ptr<IInputService> input_service = m_services.tryGet<IInputService>(); input_service.isValid()) {
            input_service->postInputMessages(dt);
        }

        return true;
    }

    void Application::render() {
    }

    void Application::terminate() {
        PPR_LOG(App, info, "terminate GLFW");
        ::glfwTerminate();
    }

}
