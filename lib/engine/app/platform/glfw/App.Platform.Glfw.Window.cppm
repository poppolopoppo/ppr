module;

export module engine.app:platform.glfw.window;

import :service.window;

export namespace pP {
    // ------------------------------------------------------------------
    // GLFW window service
    // ------------------------------------------------------------------

    class GlfwWindow : public IWindowService {
        MonitorCallback m_when_monitor_connected{};
        MonitorCallback m_when_monitor_disconnected{};

        WindowCallback m_when_window_closed{};
        WindowCallback m_when_window_created{};
        WindowCallback m_when_window_destroyed{};

        WindowFocusedCallback m_when_window_focused{};
        WindowIconifiedCallback m_when_window_iconified{};
        WindowMovedCallback m_when_window_moved{};
        WindowResizedCallback m_when_window_resized{};

    public:
        [[nodiscard]] std::expected<SharedWindow, std::errc> createWindow(
            const WindowModel &definition,
            const SharedMonitor &fullscreen,
            const SharedWindow &share_resources_with) override;

        void destroyWindow(SharedWindow &&window) override;

        void enumerateMonitors(Collector<SharedMonitor> each_monitor) const noexcept override;

        void enumerateMonitorVideoModes(Collector<VideoMode> each_video_mode) const noexcept override;

        [[nodiscard]] SharedWindow getFocusedWindow() const noexcept override;

        [[nodiscard]] SharedWindow getMainWindow() const noexcept override;

        [[nodiscard]] VideoMode getMonitorVideoMode(const Monitor &monitor) const noexcept override;

        [[nodiscard]] SharedWindow getPrimaryMonitor() const noexcept override;

        std::string_view getWindowClipboardString(const Window &window) const noexcept override;

        [[nodiscard]] SharedMonitor getWindowMonitor(const Window &window) const noexcept override;

        [[nodiscard]] bool getWindowShouldClose(const Window &window) const noexcept override;

        void hideWindow(const Window &window) override;

        void iconifyWindow(const Window &window) override;

        void moveWindow(const Window &window, const int2 &position) override;

        void pollEvents() override;

        void renameWindow(const Window &window, const std::string_view &title) override;

        void resizeWindow(const Window &window, const int2 &size) override;

        void restoreWindow(const Window &window) override;

        void setMonitorGamma(const Monitor &monitor, float gamma) noexcept override;

        void setWindowClipboardString(const Window &window, const std::string_view &text) override;

        void setWindowShouldClose(const Window &window, bool value) override;

        void showWindow(const Window &window) override;

        void swapWindowBuffers(const Window &window) override;

        void waitEvents() override;

        [[nodiscard]] MonitorCallback::Handle whenMonitorConnected(MonitorCallback::Event on_connected) noexcept override;

        [[nodiscard]] MonitorCallback::Handle whenMonitorDisconnected(MonitorCallback::Event on_disconnected) noexcept override;

        [[nodiscard]] WindowCallback::Handle whenWindowClosed(WindowCallback::Event on_connected) noexcept override;

        [[nodiscard]] WindowCallback::Handle whenWindowCreated(WindowCallback::Event on_connected) noexcept override;

        [[nodiscard]] WindowCallback::Handle whenWindowDestroyed(WindowCallback::Event on_connected) noexcept override;

        [[nodiscard]] WindowFocusedCallback::Handle whenWindowFocused(WindowFocusedCallback::Event on_focused) noexcept override;

        [[nodiscard]] WindowIconifiedCallback::Handle whenWindowIconified(WindowIconifiedCallback::Event on_iconified) noexcept override;

        [[nodiscard]] WindowMovedCallback::Handle whenWindowMoved(WindowMovedCallback::Event on_moved) noexcept override;

        [[nodiscard]] WindowResizedCallback::Handle whenWindowResized(WindowResizedCallback::Event on_resized) noexcept override;
    };
}
