module;

#include "pP/Macros.h"

module engine.app;

import :application;
import :platform;
import :service.input;
import :service.window;
import :window.handle;
import :window.monitor;
import engine.core;
import engine.rhi;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(App, debug, none)

    Application::Application(const std::string_view name, const std::span<const char *const> argv)
        : m_platform(IPlatform::get()),
          m_arguments(argv.begin(), argv.end()),
          m_name(name) {
        PPR_ASSERT(m_platform != nullptr);
    }

    Application::~Application() noexcept = default;

    void Application::setExitCode(const int exitCode) noexcept {
        m_exitCode = exitCode;
    }

    int Application::run() {
        setExitCode(exit_no_error);

        const bool initialized = initialize();
        PPR_DEFER {
            if (initialized) {
                terminate();
            }
        };

        if (initialized) [[likely]] {
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
        PPR_ASSERT(m_state == EState::created);

        PPR_LOG(App, info, "starting application", {
            {"name", m_name},
            {"platform", hal::platformName()},
            {"args", [this]() noexcept -> opaque::TransformView {
                return opaque::TransformView(m_arguments);
            }},
        });

        m_platform->initializePlatform(*this);
        if (not m_services.tryGet<IWindowService>().isValid()) [[unlikely]] {
            setExitCode(exit_failed_init);
            return false;
        }

        m_cached_window_service = m_services.get<IWindowService>();
        m_cached_input_service = m_services.tryGet<IInputService>();

        WindowModel model{};
        model.m_title = m_name;
        model.m_window_size = int2{1280, 720};
        if (auto result = m_cached_window_service->createWindow(std::move(model))) {
            m_main_window = std::move(*result);
            m_cached_window_service->setMainWindow(*m_main_window);
        } else {
            setExitCode(exit_failed_init);
            return false;
        }

        if (not rhi::initialize()) [[unlikely]] {
            setExitCode(exit_failed_init);
            return false;
        }

        auto [ctx, cancelFn] = context::withCancel(context::background());
        m_lifecycle = std::move(ctx);
        m_cancel = std::move(cancelFn);

        m_state = EState::initialized;
        return true;
    }

    bool Application::update() {
        if (m_lifecycle->error()) [[unlikely]] {
            return false;
        }

        PPR_ASSERT(m_cached_window_service.isValid());
        m_cached_window_service->pollEvents();

        PPR_ASSERT(m_main_window.isValid());
        if (m_cached_window_service->getWindowShouldClose(*m_main_window)) [[unlikely]] {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const TimeSpan dt = now - m_last_frame_time;
        m_last_frame_time = now;

        if (m_cached_input_service.isValid()) {
            m_cached_input_service->postInputMessages(dt);
        }

        return true;
    }

    void Application::render() {
        PPR_ASSERT(m_cached_window_service.isValid());
        PPR_ASSERT(m_main_window.isValid());
        m_cached_window_service->swapWindowBuffers(*m_main_window);
    }

    void Application::terminate() noexcept {
        if (m_state >= EState::terminated) [[unlikely]] {
            return;
        }
        m_state = EState::terminated;
        m_cancel();
        m_main_window.reset();
        m_cached_window_service.reset();
        m_cached_input_service.reset();
        try {
            m_platform->shutdownPlatform(*this);
        } catch (...) {
            PPR_LOG(App, error, "exception during platform shutdown");
        }
    }
}
