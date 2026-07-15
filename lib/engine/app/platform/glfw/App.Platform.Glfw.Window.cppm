module;
#include <GLFW/glfw3.h>
#include "pP/Macros.h"
export module engine.app:platform.glfw.window;

import :service.window;
import :platform.glfw.input;
import engine.core;

export namespace pP {
    class GlfwWindow final : public IWindowService {
        void initializeMonitors_();

        void initializeGlobalCallbacks_();

        GlfwWindow() noexcept = default;

    public:
        MonitorCallback m_when_monitor_connected{};
        MonitorCallback m_when_monitor_disconnected{};

        WindowCallback m_when_window_closed{};
        WindowCallback m_when_window_created{};
        WindowCallback m_when_window_destroyed{};

        WindowFocusedCallback m_when_window_focused{};
        WindowIconifiedCallback m_when_window_iconified{};
        WindowMovedCallback m_when_window_moved{};
        WindowResizedCallback m_when_window_resized{};
        WindowScaledCallback m_when_window_scaled{};

        Array<std::unique_ptr<Window> > m_windows{};
        Array<std::unique_ptr<Monitor> > m_monitors{};

        safe_ptr<Monitor> m_primary_monitor{};
        safe_ptr<Window> m_main_window{};
        safe_ptr<Window> m_focused_window{};
        safe_ptr<GlfwInput> m_input_service{};
        bool m_shutdown{false};

        [[nodiscard]] static GlfwWindow &get() noexcept;

        void initialize();

        void shutdown();

        void setInputService(safe_ptr<GlfwInput> input_service) noexcept;

        // ------------------------------------------------------------------
        // IWindowService overrides
        // ------------------------------------------------------------------

        // events:
        void pollEvents() override;

        void waitEvents() override;

        // monitors:
        void enumerateMonitors(Collector<SharedMonitor> each_monitor) const noexcept override;

        [[nodiscard]] SharedMonitor getPrimaryMonitor() const noexcept override;

        void enumerateMonitorVideoModes(const Monitor &monitor, Collector<VideoMode> each_video_mode) const noexcept override;

        void setMonitorGamma(const Monitor &monitor, float gamma) noexcept override;

        // monitor callbacks:
        [[nodiscard]] MonitorCallback::Handle whenMonitorConnected(MonitorCallback::Event on_connected) noexcept override;

        [[nodiscard]] MonitorCallback::Handle whenMonitorDisconnected(MonitorCallback::Event on_disconnected) noexcept override;

        // windows:
        [[nodiscard]] std::expected<SharedWindow, std::error_code> createWindow(
            WindowModel &&definition,
            const SharedMonitor &fullscreen,
            const SharedWindow &share_resources_with) override;

        void destroyWindow(SharedWindow &&window) override;

        [[nodiscard]] SharedWindow getFocusedWindow() const noexcept override;

        [[nodiscard]] SharedWindow getMainWindow() const noexcept override;

        void setMainWindow(const Window &window) override;

        [[nodiscard]] SharedMonitor getWindowMonitor(const Window &window) const noexcept override;

        void setWindowMonitor(
            const Window &window,
            const Monitor &monitor,
            const int2 &window_position,
            const int2 &window_size) override;

        [[nodiscard]] bool getWindowShouldClose(const Window &window) const noexcept override;

        void setWindowShouldClose(const Window &window, bool value) override;

        // window manipulation:
        void moveWindow(const Window &window, const int2 &position) override;

        void resizeWindow(const Window &window, const int2 &size) override;

        void renameWindow(const Window &window, const std::string_view &title) override;

        void showWindow(const Window &window) override;

        void hideWindow(const Window &window) override;

        void iconifyWindow(const Window &window) override;

        void restoreWindow(const Window &window) override;

        void swapWindowBuffers(const Window &window) override;

        // window clipboard:
        void setWindowClipboardString(const Window &window, const std::string_view &text) override;

        std::string_view getWindowClipboardString(const Window &window) const noexcept override;

        // window callbacks:
        [[nodiscard]] WindowCallback::Handle whenWindowCreated(WindowCallback::Event on_connected) noexcept override;

        [[nodiscard]] WindowCallback::Handle whenWindowDestroyed(WindowCallback::Event on_destroyed) noexcept override;

        [[nodiscard]] WindowCallback::Handle whenWindowClosed(WindowCallback::Event on_closed) noexcept override;

        [[nodiscard]] WindowFocusedCallback::Handle whenWindowFocused(WindowFocusedCallback::Event on_focused) noexcept override;

        [[nodiscard]] WindowIconifiedCallback::Handle whenWindowIconified(WindowIconifiedCallback::Event on_iconified) noexcept override;

        [[nodiscard]] WindowMovedCallback::Handle whenWindowMoved(WindowMovedCallback::Event on_moved) noexcept override;

        [[nodiscard]] WindowResizedCallback::Handle whenWindowResized(WindowResizedCallback::Event on_resized) noexcept override;

        [[nodiscard]] WindowScaledCallback::Handle whenWindowScaled(WindowScaledCallback::Event on_scaled) noexcept override;
    };
}
