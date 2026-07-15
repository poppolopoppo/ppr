module;

export module engine.app:service.window;

import engine.core;
import engine.math;
import std;

export namespace pP {
    class Monitor;
    struct VideoMode;
    using MonitorHandle = Numeric<void *, Monitor>;
    using SharedMonitor = safe_ptr<const Monitor>;

    class Window;
    struct WindowModel;
    using WindowHandle = Numeric<void *, Window>;
    using SharedWindow = safe_ptr<const Window>;

    // ------------------------------------------------------------------
    // window service interface
    // ------------------------------------------------------------------

    class IWindowService : public virtual IService {
    public:
        // events:
        virtual void pollEvents() = 0;

        virtual void waitEvents() = 0;

        // monitors:
        virtual void enumerateMonitors(Collector<SharedMonitor> each_monitor) const noexcept = 0;

        [[nodiscard]] virtual SharedMonitor getPrimaryMonitor() const noexcept = 0;

        virtual void enumerateMonitorVideoModes(const Monitor &monitor, Collector<VideoMode> each_video_mode) const noexcept = 0;

        virtual void setMonitorGamma(const Monitor &monitor, float gamma) noexcept = 0;

        // monitor callbacks:

        using MonitorCallback = Callback<void(const Monitor &monitor)>;

        [[nodiscard]] virtual MonitorCallback::Handle whenMonitorConnected(MonitorCallback::Event on_connected) noexcept = 0;

        [[nodiscard]] virtual MonitorCallback::Handle whenMonitorDisconnected(MonitorCallback::Event on_disconnected) noexcept = 0;

        // windows:
        [[nodiscard]] virtual std::expected<SharedWindow, std::error_code> createWindow(
            WindowModel &&definition,
            const SharedMonitor &fullscreen = {},
            const SharedWindow &share_resources_with = {}) = 0;

        virtual void destroyWindow(SharedWindow &&window) = 0;

        [[nodiscard]] virtual SharedWindow getFocusedWindow() const noexcept = 0;

        [[nodiscard]] virtual SharedWindow getMainWindow() const noexcept = 0;

        virtual void setMainWindow(const Window &window) = 0;

        [[nodiscard]] virtual SharedMonitor getWindowMonitor(const Window &window) const noexcept = 0;

        virtual void setWindowMonitor(
            const Window &window,
            const Monitor &monitor,
            const int2 &window_position,
            const int2 &window_size) = 0;

        [[nodiscard]] virtual bool getWindowShouldClose(const Window &window) const noexcept = 0;

        virtual void setWindowShouldClose(const Window &window, bool value) = 0;

        // window manipulation:
        virtual void moveWindow(const Window &window, const int2 &position) = 0;

        virtual void resizeWindow(const Window &window, const int2 &size) = 0;

        virtual void renameWindow(const Window &window, const std::string_view &title) = 0;

        virtual void showWindow(const Window &window) = 0;

        virtual void hideWindow(const Window &window) = 0;

        virtual void iconifyWindow(const Window &window) = 0;

        virtual void restoreWindow(const Window &window) = 0;

        virtual void swapWindowBuffers(const Window &window) = 0;

        // window clipboard:
        virtual void setWindowClipboardString(const Window &window, const std::string_view &text) = 0;

        virtual std::string_view getWindowClipboardString(const Window &window) const noexcept = 0;

        // window callbacks:

        using WindowCallback = Callback<void(const Window &window)>;

        [[nodiscard]] virtual WindowCallback::Handle whenWindowCreated(WindowCallback::Event on_connected) noexcept = 0;

        [[nodiscard]] virtual WindowCallback::Handle whenWindowDestroyed(WindowCallback::Event on_destroyed) noexcept = 0;

        [[nodiscard]] virtual WindowCallback::Handle whenWindowClosed(WindowCallback::Event on_closed) noexcept = 0;

        using WindowFocusedCallback = Callback<void(const Window &window, bool focused)>;

        [[nodiscard]] virtual WindowFocusedCallback::Handle whenWindowFocused(WindowFocusedCallback::Event on_focused) noexcept = 0;

        using WindowIconifiedCallback = Callback<void(const Window &window, bool iconified)>;

        [[nodiscard]] virtual WindowIconifiedCallback::Handle whenWindowIconified(WindowIconifiedCallback::Event on_iconified) noexcept = 0;

        using WindowMovedCallback = Callback<void(const Window &window, const int2 &old_position)>;

        [[nodiscard]] virtual WindowMovedCallback::Handle whenWindowMoved(WindowMovedCallback::Event on_moved) noexcept = 0;

        using WindowResizedCallback = Callback<void(const Window &window, const int2 &old_size)>;

        [[nodiscard]] virtual WindowResizedCallback::Handle whenWindowResized(WindowResizedCallback::Event on_resized) noexcept = 0;

        using WindowScaledCallback = Callback<void(const Window &window, const float2 &old_scale)>;

        [[nodiscard]] virtual WindowScaledCallback::Handle whenWindowScaled(WindowScaledCallback::Event on_scaled) noexcept = 0;
    };
}
