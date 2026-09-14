module;

#include "pP/Macros.h"
export module engine.app:platform.glfw.window;

import :service.window;
import engine.core;

export namespace pP {
    class GlfwWindow final : public IWindowService {
        [[nodiscard]] std::error_code initializeMonitors_();

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

        safe_ptr<const Monitor> m_primary_monitor{};
        safe_ptr<const Window> m_main_window{};
        safe_ptr<const Window> m_focused_window{};
        safe_ptr<const WindowViewport> m_main_viewport{};

        std::error_code initialize();

        std::error_code shutdown();

        // ------------------------------------------------------------------
        // IWindowService overrides
        // ------------------------------------------------------------------

        // events:
        [[nodiscard]] std::error_code pollEvents() override;

        [[nodiscard]] std::error_code waitEvents() override;

        // monitors:
        [[nodiscard]] std::error_code enumerateMonitors(Collector<SharedMonitor> each_monitor) const noexcept override;

        [[nodiscard]] SharedMonitor getPrimaryMonitor() const noexcept override;

        [[nodiscard]] std::error_code enumerateMonitorVideoModes(const Monitor &monitor, Collector<VideoMode> each_video_mode) const noexcept override;

        void setMonitorGamma(const Monitor &monitor, float gamma) noexcept override;

        // monitor callbacks:
        [[nodiscard]] MonitorCallback::Handle whenMonitorConnected(MonitorCallback::Event on_connected) noexcept override;

        [[nodiscard]] MonitorCallback::Handle whenMonitorDisconnected(MonitorCallback::Event on_disconnected) noexcept override;

        // windows:
        [[nodiscard]] std::error_code createWindow(
            WindowModel &&definition,
            safe_ptr<Window> *window_write_ref) override;

        [[nodiscard]] std::error_code destroyWindow(SharedWindow &&window) override;

        [[nodiscard]] const SharedWindow &getFocusedWindow() const noexcept override { return m_focused_window; }
        [[nodiscard]] const SharedWindow &getMainWindow() const noexcept override { return m_main_window; }

        SharedWindow setMainWindow(SharedWindow window) override;

        [[nodiscard]] SharedMonitor getWindowMonitor(const Window &window) const noexcept override;

        void setWindowMonitor(
            const Window &window,
            const Monitor &monitor,
            const int2 &window_position,
            const int2 &window_size) override;

        [[nodiscard]] bool getWindowShouldClose(const Window &window) const noexcept override;

        void setWindowShouldClose(const Window &window, bool value) override;

        void setWindowCursorMode(const Window &window, ECursorMode mode) override;

        // window manipulation:
        void moveWindow(const Window &window, const int2 &position) override;

        void resizeWindow(const Window &window, const int2 &size) override;

        void renameWindow(const Window &window, const std::string_view &title) override;

        void showWindow(const Window &window) override;

        void hideWindow(const Window &window) override;

        void iconifyWindow(const Window &window) override;

        void restoreWindow(const Window &window) override;

        void focusWindow(const Window &window) override;

        void swapWindowBuffers(const Window &window) override;

        [[nodiscard]] void *getWindowNativeHandle(const Window &window) const noexcept override;

        // window clipboard:
        void setWindowClipboardString(const Window &window, const std::string_view &text) override;

        [[nodiscard]] std::string_view getWindowClipboardString(const Window &window) const noexcept override;

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
