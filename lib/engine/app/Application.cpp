module;

#if !defined(PPR_NULL_PLATFORM)
#include <GLFW/glfw3.h>
#endif

#include "pP/Macros.h"

module engine.app;

import std;
import engine.core;
import engine.rhi;
import engine.platform;
import engine.math;

namespace pP {
    using math::operator*;
    using math::operator-;

    PPR_DEFINE_LOG_CATEGORY(App, debug, none)

    math::float4x4 Camera::viewMatrix() const noexcept {
        return math::lookAt(
            math::float3{position.x, position.y, position.z},
            math::float3{target.x, target.y, target.z},
            math::float3{up.x, up.y, up.z}
        );
    }

    math::float4x4 Camera::projectionMatrix() const noexcept {
        return math::perspectiveD3D(fovY, aspect, nearZ, farZ);
    }

    math::float4x4 Camera::viewProjectionMatrix() const noexcept {
        return projectionMatrix() * viewMatrix();
    }

    std::pair<math::float3, math::float3> Camera::screenToWorld(math::float2 screenPos) const noexcept {
        auto proj = projectionMatrix();
        auto view = viewMatrix();
        auto invVP = math::inverse(proj * view);

        math::float4 nearNDC{screenPos.x, screenPos.y, 0.0f, 1.0f};
        math::float4 farNDC{ screenPos.x, screenPos.y, 1.0f, 1.0f};

        auto nearWorld = nearNDC * invVP;
        auto farWorld  = farNDC * invVP;

        math::float3 origin{nearWorld.x / nearWorld.w, nearWorld.y / nearWorld.w, nearWorld.z / nearWorld.w};
        math::float3 farPt{ farWorld.x / farWorld.w,  farWorld.y / farWorld.w,  farWorld.z / farWorld.w };
        math::float3 direction = math::normalize(farPt - origin);

        return {origin, direction};
    }

    Application::Application(const std::string_view name, const std::span<const char* const> argv)
        : m_arguments(argv.begin(), argv.end()),
          m_name(name) {
    }

    Application::~Application() noexcept = default;

    void Application::setExitCode(const int exitCode) noexcept {
        m_exitCode = exitCode;
    }

    int Application::run() {
        setExitCode(exit_no_error);

        if (onInitialize()) [[likely]] {
            PPR_DEFER { onShutdown(); };

            if (!m_platform) [[unlikely]]
                return m_exitCode;

            while (true) {
                double now = m_platform->getTime();
                double dt = now - m_lastFrameTime;
                m_lastFrameTime = now;

                if (!onUpdate(dt)) break;
                onRender();
            }
        }

        return m_exitCode;
    }

    bool Application::onInitialize() {
        PPR_LOG(App, info, "starting application", {
            {"name", m_name},
            {"platform", hal::platformName()},
            {"args", [this]() noexcept -> opaque::TransformView {
                return opaque::TransformView(m_arguments);
            }},
        });

#if defined(PPR_NULL_PLATFORM)
        auto platform = std::make_unique<detail::NullPlatform>();
#else
        ::glfwSetErrorCallback([](const int error_code, const char *const description) {
            PPR_LOG(App, error, "caught GLFW error", {{"error_code", error_code}, {"description", description}});
        });

        constexpr ::GLFWallocator glfw_allocator{
            .allocate = [](const std::size_t size, [[maybe_unused]] void *user) -> void * { return std::malloc(size); },
            .reallocate = [](void *const block, const std::size_t size, [[maybe_unused]] void *user) -> void * { return std::realloc(block, size); },
            .deallocate = [](void *const block, [[maybe_unused]] void *user) -> void { std::free(block); },
            .user = nullptr,
        };
        ::glfwInitAllocator(&glfw_allocator);

        auto platform = std::make_unique<detail::GlfwPlatform>();
        if (!platform->initialize()) {
            setExitCode(exit_failed_init);
            return false;
        }
        PPR_LOG(App, info, "successfully initialized GLFW",
            {{"version", ::glfwGetVersionString()}});
#endif

        if (not rhi::initialize()) [[unlikely]] {
            setExitCode(exit_failed_init);
            PPR_LOG(App, error, "failed to init RHI", {});
            return false;
        }
        PPR_LOG(App, info, "successfully initialized RHI", {});

        WindowDesc wdesc;
        wdesc.title = getName();
        auto window = platform->createWindow(wdesc);
        if (!window) {
            setExitCode(exit_failed_init);
            return false;
        }

        m_platform = std::move(platform);
        m_window = std::move(*window);
        m_lastFrameTime = m_platform->getTime();
        return true;
    }

    bool Application::onUpdate(double /*deltaTime*/) {
        m_platform->processEvents();
        return !m_window->shouldClose();
    }

    void Application::onRender() {
        m_window->swapBuffers();
    }

    void Application::onShutdown() {
        m_window.reset();
        m_platform.reset();
    }

    IPlatform& Application::getPlatform() const noexcept {
        return *m_platform;
    }

    IWindow& Application::getWindow() const noexcept {
        return *m_window;
    }

    Camera& Application::getCamera() noexcept {
        return m_camera;
    }

}
