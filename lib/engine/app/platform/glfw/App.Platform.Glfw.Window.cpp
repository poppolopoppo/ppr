module;
#include <GLFW/glfw3.h>

#include "pP/Macros.h"

module engine.app;

import :platform.glfw.window;
import :platform.glfw.input;
import :window.handle;
import :window.monitor;

namespace pP {
    // ------------------------------------------------------------------
    // GLFW static helpers
    // ------------------------------------------------------------------

    [[nodiscard]] static ::GLFWmonitor *glfwHandle_(const MonitorHandle &monitor_handle) noexcept {
        PPR_ASSERT(monitor_handle != nullptr && "invalid GLFW monitor handle");
        return static_cast<::GLFWmonitor *>(monitor_handle.m_value);
    }

    [[nodiscard]] static ::GLFWwindow *glfwHandle_(const WindowHandle &window_handle) noexcept {
        PPR_ASSERT(window_handle != nullptr && "invalid GLFW window handle");
        return static_cast<::GLFWwindow *>(window_handle.m_value);
    }

    [[nodiscard]] static VideoMode glfwVideoMode_(const GLFWvidmode &mode) noexcept {
        return VideoMode{
            .m_resolution = int2(mode.width, mode.height),
            .m_rgb_bits = int3(mode.redBits, mode.greenBits, mode.blueBits),
            .m_refresh_rate = mode.refreshRate,
        };
    }

    template<typename T>
    [[nodiscard]] static auto glfwAllocation_(const Array<std::unique_ptr<T> > &allocations, const T &shared) noexcept {
        return std::ranges::find_if(allocations, [&](const std::unique_ptr<T> &owned) noexcept -> bool {
            return owned.get() == &shared;
        });
    }

    template<typename T>
    [[nodiscard]] static auto glfwAllocation_(const Array<std::unique_ptr<T> > &allocations, const Numeric<void *, T> handle) noexcept {
        return std::ranges::find_if(allocations, [&](const std::unique_ptr<T> &owned) noexcept -> bool {
            return owned->m_handle == handle;
        });
    }

    [[nodiscard]] static std::unique_ptr<Monitor> glfwCreateMonitor_(GLFWmonitor *const p_glfw_monitor) {
        std::string monitor_name = ::glfwGetMonitorName(p_glfw_monitor);

        int2 monitor_physical_size{};
        ::glfwGetMonitorPhysicalSize(p_glfw_monitor, &monitor_physical_size.x, &monitor_physical_size.y);

        int2 monitor_virtual_position{};
        ::glfwGetMonitorPos(p_glfw_monitor, &monitor_virtual_position.x, &monitor_virtual_position.y);

        const GLFWvidmode *p_video_mode = ::glfwGetVideoMode(p_glfw_monitor);
        PPR_ASSERT(p_video_mode != nullptr);

        auto monitor = std::make_unique<Monitor>(
            MonitorHandle{p_glfw_monitor},
            std::move(monitor_name),
            glfwVideoMode_(*p_video_mode),
            monitor_physical_size,
            monitor_virtual_position,
            ::glfwGetPrimaryMonitor() == p_glfw_monitor);

        ::glfwSetMonitorUserPointer(p_glfw_monitor, monitor.get());

        return std::move(monitor);
    }

    // ------------------------------------------------------------------
    // GLFW window service init/destroy
    // ------------------------------------------------------------------

    /*static*/
    GlfwWindow &GlfwWindow::get() noexcept {
        static GlfwWindow g_instance;
        return g_instance;
    }

    void GlfwWindow::initialize() {
        initializeMonitors_();
        initializeGlobalCallbacks_();
    }

    void GlfwWindow::setInputService(safe_ptr<GlfwInput> input_service) noexcept {
        m_input_service = std::move(input_service);
    }

    void GlfwWindow::initializeMonitors_() {
        int monitors_count{0};
        GLFWmonitor **const p_monitors_arr = ::glfwGetMonitors(&monitors_count);
        PPR_ASSERT(p_monitors_arr != nullptr || monitors_count > 0);

        m_monitors.reserve(safe_narrowing(monitors_count));
        for (int i = 0; i < monitors_count; ++i) {
            if (PPR_ENSURE(p_monitors_arr[i])) {
                std::unique_ptr<Monitor> monitor = glfwCreateMonitor_(p_monitors_arr[i]);

                if (monitor->m_primary_monitor) {
                    m_primary_monitor.reset(monitor.get());
                }

                m_monitors.push_back(std::move(monitor));
            }
        }
    }

    static void glfwMonitorCallback_(GLFWmonitor *p_glfw_monitor, const int status) {
        PPR_ASSERT(p_glfw_monitor != nullptr);
        GlfwWindow &windows = GlfwWindow::get();

        if (status == GLFW_CONNECTED) {
            std::unique_ptr<Monitor> monitor = glfwCreateMonitor_(p_glfw_monitor);

            if (monitor->m_primary_monitor) {
                windows.m_primary_monitor.reset(monitor.get());
            }

            windows.m_monitors.push_back(std::move(monitor));
            windows.m_when_monitor_connected(*windows.m_monitors.back());
            return;
        }

        if (status == GLFW_DISCONNECTED) {
            if (const auto it = glfwAllocation_(windows.m_monitors, MonitorHandle{p_glfw_monitor});
                windows.m_monitors.end() != it) {
                windows.m_when_monitor_disconnected(**it);
                windows.m_monitors.erase(it);
            }
            return;
        }

        std::unreachable();
    }

    void GlfwWindow::initializeGlobalCallbacks_() {
        ::glfwSetMonitorCallback(&glfwMonitorCallback_);
    }

    // ------------------------------------------------------------------
    // GLFW window events
    // ------------------------------------------------------------------

    void GlfwWindow::pollEvents() {
        ::glfwPollEvents();
    }

    void GlfwWindow::waitEvents() {
        ::glfwWaitEvents();
    }

    // ------------------------------------------------------------------
    // GLFW monitors
    // ------------------------------------------------------------------

    void GlfwWindow::enumerateMonitors(const Collector<SharedMonitor> each_monitor) const noexcept {
        for (const std::unique_ptr<Monitor> &monitor: m_monitors) {
            each_monitor(SharedMonitor(monitor.get()));
        }
    }

    [[nodiscard]] SharedMonitor GlfwWindow::getPrimaryMonitor() const noexcept {
        return m_primary_monitor;
    }

    void GlfwWindow::enumerateMonitorVideoModes(const Monitor &monitor, Collector<VideoMode> each_video_mode) const noexcept {
        int video_modes_count{0};
        const ::GLFWvidmode *const p_video_modes_arr = ::glfwGetVideoModes(glfwHandle_(monitor.m_handle), &video_modes_count);
        PPR_ASSERT(p_video_modes_arr != nullptr || video_modes_count == 0);
        if (p_video_modes_arr == nullptr) {
            return;
        }

        for (int i = 0; i < video_modes_count; ++i) {
            each_video_mode(glfwVideoMode_(p_video_modes_arr[i]));
        }
    }

    void GlfwWindow::setMonitorGamma(const Monitor &monitor, float gamma) noexcept {
        if (monitor.m_handle) {
            ::glfwSetGamma(glfwHandle_(monitor.m_handle), gamma);
        }
    }

    // ------------------------------------------------------------------
    // GLFW monitor callbacks
    // ------------------------------------------------------------------

    auto GlfwWindow::whenMonitorConnected(MonitorCallback::Event on_connected) noexcept -> MonitorCallback::Handle {
        return m_when_monitor_connected.add(std::move(on_connected));
    }

    auto GlfwWindow::whenMonitorDisconnected(MonitorCallback::Event on_disconnected) noexcept -> MonitorCallback::Handle {
        return m_when_monitor_disconnected.add(std::move(on_disconnected));
    }

    // ------------------------------------------------------------------
    // GLFW windows
    // ------------------------------------------------------------------

    [[nodiscard]] static Window &getWindowFromGlfwHandle_(::GLFWwindow *p_glfw_window) noexcept {
        PPR_ASSERT(p_glfw_window != nullptr);
        return *static_cast<Window *>(::glfwGetWindowUserPointer(p_glfw_window));
    }

    static void glfwWindowCloseCallback_(::GLFWwindow *p_glfw_window) noexcept {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        window.m_visible = false;
        window.m_when_closed(window);
    }

    static void glfwWindowFocusCallback_(::GLFWwindow *p_glfw_window, const int focused) noexcept {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        window.m_focused = focused == GLFW_TRUE;
        window.m_when_focused(window, window.m_focused);

        GlfwWindow &g_instance = GlfwWindow::get();
        if (window.m_focused) {
            g_instance.m_focused_window = safe_ptr(&window);
        } else if (g_instance.m_focused_window == &window) {
            g_instance.m_focused_window = nullptr;
        }
    }

    static void glfwWindowIconifyCallback_(::GLFWwindow *p_glfw_window, const int iconified) noexcept {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        window.m_iconified = iconified == GLFW_TRUE;
        window.m_when_iconified(window, window.m_iconified);
    }

    static void glfwWindowPosCallback_(::GLFWwindow *p_glfw_window, const int position_x, const int position_y) noexcept {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const int2 old_position = window.m_window_position;
        window.m_window_position = int2{position_x, position_y};
        window.m_when_moved(window, old_position);
    }

    static void glfwWindowSizeCallback_(::GLFWwindow *p_glfw_window, const int size_x, const int size_y) noexcept {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const int2 old_size = window.m_window_size;
        window.m_window_size = int2{size_x, size_y};
        window.m_when_resized(window, old_size);
    }

    static void glfwFramebufferSizeCallback_(::GLFWwindow *p_glfw_window, const int size_x, const int size_y) noexcept {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const int2 old_framebuffer_size = window.m_framebuffer_size;
        window.m_framebuffer_size = int2{size_x, size_y};
        window.m_when_resized(window, old_framebuffer_size);
    }

    static void glfwWindowContentScaleCallback_(::GLFWwindow *p_glfw_window, const float scale_x, const float scale_y) noexcept {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const float2 old_content_scale = window.m_content_scale;
        window.m_content_scale = float2{scale_x, scale_y};
        window.m_when_scaled(window, old_content_scale);
    }

    static void glfwKeyCallback_(::GLFWwindow *, const int key, const int scancode, const int action, const int mods) noexcept {
        GlfwInput::get().onKey(key, scancode, action, mods);
    }

    static void glfwCharCallback_(::GLFWwindow *, const unsigned int codepoint) noexcept {
        GlfwInput::get().onChar(codepoint);
    }

    static void glfwMouseButtonCallback_(::GLFWwindow *, const int button, const int action, const int mods) noexcept {
        GlfwInput::get().onMouseButton(button, action, mods);
    }

    static void glfwCursorPosCallback_(::GLFWwindow *, const double x, const double y) noexcept {
        GlfwInput::get().onCursorPos(x, y);
    }

    static void glfwScrollCallback_(::GLFWwindow *, const double x_offset, const double y_offset) noexcept {
        GlfwInput::get().onScroll(x_offset, y_offset);
    }

    std::expected<SharedWindow, std::error_code> GlfwWindow::createWindow(
        WindowModel &&definition,
        const SharedMonitor &fullscreen,
        const SharedWindow &share_resources_with) {
        GLFWwindow *p_glfw_window = ::glfwCreateWindow(
            definition.m_window_size.x,
            definition.m_window_size.y,
            definition.m_title.data(),
            fullscreen.isValid() ? glfwHandle_(fullscreen->m_handle) : nullptr,
            share_resources_with.isValid() ? glfwHandle_(share_resources_with->m_handle) : nullptr);

        if (p_glfw_window == nullptr) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }

        PPR_DEFER {
            if (p_glfw_window) {
                ::glfwDestroyWindow(p_glfw_window);
            }
        };

        ::glfwSetWindowAttrib(p_glfw_window, GLFW_FOCUSED, definition.m_focused);
        ::glfwSetWindowAttrib(p_glfw_window, GLFW_DECORATED, definition.m_decorated);
        ::glfwSetWindowAttrib(p_glfw_window, GLFW_RESIZABLE, definition.m_resizable);
        ::glfwSetWindowAttrib(p_glfw_window, GLFW_VISIBLE, definition.m_visible);

        int2 framebuffer_size{};
        ::glfwGetFramebufferSize(p_glfw_window, &framebuffer_size.x, &framebuffer_size.y);

        int2 window_position{};
        ::glfwGetWindowPos(p_glfw_window, &window_position.x, &window_position.y);

        int2 window_size{};
        ::glfwGetWindowSize(p_glfw_window, &window_size.x, &window_size.y);

        float2 content_scale{1.0};
        ::glfwGetWindowContentScale(p_glfw_window, &content_scale.x, &content_scale.y);

        auto window = std::make_unique<Window>(WindowHandle{p_glfw_window}, std::move(definition));
        window->m_content_scale = content_scale;
        window->m_framebuffer_size = framebuffer_size;
        window->m_window_position = window_position;
        window->m_window_size = window_size;

        ::glfwSetWindowUserPointer(p_glfw_window, window.get());

        ::glfwSetWindowCloseCallback(p_glfw_window, &glfwWindowCloseCallback_);
        ::glfwSetWindowFocusCallback(p_glfw_window, &glfwWindowFocusCallback_);
        ::glfwSetWindowIconifyCallback(p_glfw_window, &glfwWindowIconifyCallback_);
        ::glfwSetWindowPosCallback(p_glfw_window, &glfwWindowPosCallback_);
        ::glfwSetWindowSizeCallback(p_glfw_window, &glfwWindowSizeCallback_);
        ::glfwSetFramebufferSizeCallback(p_glfw_window, &glfwFramebufferSizeCallback_);
        ::glfwSetWindowContentScaleCallback(p_glfw_window, &glfwWindowContentScaleCallback_);

        ::glfwSetKeyCallback(p_glfw_window, &glfwKeyCallback_);
        ::glfwSetCharCallback(p_glfw_window, &glfwCharCallback_);
        ::glfwSetMouseButtonCallback(p_glfw_window, &glfwMouseButtonCallback_);
        ::glfwSetCursorPosCallback(p_glfw_window, &glfwCursorPosCallback_);
        ::glfwSetScrollCallback(p_glfw_window, &glfwScrollCallback_);

        p_glfw_window = nullptr;

        SharedWindow shared_window{window.get()};
        m_windows.push_back(std::move(window));

        m_when_window_created(*shared_window);
        return shared_window;
    }

    void GlfwWindow::destroyWindow(SharedWindow &&window) {
        if (const auto it = glfwAllocation_(m_windows, *window);
            PPR_ENSURE(m_windows.end() != it)) {
            m_when_window_destroyed(**it);

            if (m_main_window == window) {
                m_main_window = nullptr;
            }

            if (m_focused_window == window) {
                m_focused_window = nullptr;
            }

            m_windows.erase(it);
        }
    }

    [[nodiscard]] SharedWindow GlfwWindow::getFocusedWindow() const noexcept {
        return m_focused_window;
    }

    [[nodiscard]] SharedWindow GlfwWindow::getMainWindow() const noexcept {
        return m_main_window;
    }

    void GlfwWindow::setMainWindow(const Window &window) {
        if (const auto it = glfwAllocation_(m_windows, window);
            m_windows.end() != it) [[likely]] {
            m_main_window = safe_ptr(it->get());
        }
    }

    [[nodiscard]] SharedMonitor GlfwWindow::getWindowMonitor(const Window &window) const noexcept {
        if (window.m_handle) [[likely]] {
            if (::GLFWmonitor *const p_glfw_monitor = ::glfwGetWindowMonitor(glfwHandle_(window.m_handle))) {
                const auto it = glfwAllocation_(m_monitors, MonitorHandle{p_glfw_monitor});
                if (PPR_ENSURE(m_monitors.end() != it)) {
                    return SharedMonitor{it->get()};
                }
            }
        }
        return SharedMonitor{};
    }

    void GlfwWindow::setWindowMonitor(
        const Window &window,
        const Monitor &monitor,
        const int2 &window_position,
        const int2 &window_size) {
        ::glfwSetWindowMonitor(
            glfwHandle_(window.m_handle),
            glfwHandle_(monitor.m_handle),
            window_position.x,
            window_position.y,
            window_size.x,
            window_size.y,
            GLFW_DONT_CARE);
    }

    [[nodiscard]] bool GlfwWindow::getWindowShouldClose(const Window &window) const noexcept {
        return ::glfwWindowShouldClose(glfwHandle_(window.m_handle));
    }

    // ------------------------------------------------------------------
    // GLFW window manipulation
    // ------------------------------------------------------------------

    void GlfwWindow::setWindowShouldClose(const Window &window, const bool value) {
        ::glfwSetWindowShouldClose(glfwHandle_(window.m_handle), value);
    }

    void GlfwWindow::moveWindow(const Window &window, const int2 &position) {
        ::glfwSetWindowPos(glfwHandle_(window.m_handle), position.x, position.y);
    }

    void GlfwWindow::resizeWindow(const Window &window, const int2 &size) {
        ::glfwSetWindowSize(glfwHandle_(window.m_handle), size.x, size.y);
    }

    void GlfwWindow::renameWindow(const Window &window, const std::string_view &title) {
        ::glfwSetWindowTitle(glfwHandle_(window.m_handle), title.data());
    }

    void GlfwWindow::showWindow(const Window &window) {
        ::glfwShowWindow(glfwHandle_(window.m_handle));
    }

    void GlfwWindow::hideWindow(const Window &window) {
        ::glfwHideWindow(glfwHandle_(window.m_handle));
    }

    void GlfwWindow::iconifyWindow(const Window &window) {
        ::glfwIconifyWindow(glfwHandle_(window.m_handle));
    }

    void GlfwWindow::restoreWindow(const Window &window) {
        ::glfwRestoreWindow(glfwHandle_(window.m_handle));
    }

    void GlfwWindow::swapWindowBuffers(const Window &window) {
        ::glfwSwapBuffers(glfwHandle_(window.m_handle));
    }

    // ------------------------------------------------------------------
    // GLFW window clipboard
    // ------------------------------------------------------------------

    void GlfwWindow::setWindowClipboardString(const Window &window, const std::string_view &text) {
        ::glfwSetClipboardString(glfwHandle_(window.m_handle), text.data());
    }

    std::string_view GlfwWindow::getWindowClipboardString(const Window &window) const noexcept {
        return ::glfwGetClipboardString(glfwHandle_(window.m_handle));
    }

    // ------------------------------------------------------------------
    // GLFW window callbacks
    // ------------------------------------------------------------------

    auto GlfwWindow::whenWindowCreated(WindowCallback::Event on_connected) noexcept -> WindowCallback::Handle {
        return m_when_window_created.add(std::move(on_connected));
    }

    auto GlfwWindow::whenWindowDestroyed(WindowCallback::Event on_destroyed) noexcept -> WindowCallback::Handle {
        return m_when_window_destroyed.add(std::move(on_destroyed));
    }

    auto GlfwWindow::whenWindowClosed(WindowCallback::Event on_closed) noexcept -> WindowCallback::Handle {
        return m_when_window_closed.add(std::move(on_closed));
    }

    auto GlfwWindow::whenWindowFocused(WindowFocusedCallback::Event on_focused) noexcept -> WindowFocusedCallback::Handle {
        return m_when_window_focused.add(std::move(on_focused));
    }

    auto GlfwWindow::whenWindowIconified(WindowIconifiedCallback::Event on_iconified) noexcept -> WindowIconifiedCallback::Handle {
        return m_when_window_iconified.add(std::move(on_iconified));
    }

    auto GlfwWindow::whenWindowMoved(WindowMovedCallback::Event on_moved) noexcept -> WindowMovedCallback::Handle {
        return m_when_window_moved.add(std::move(on_moved));
    }

    auto GlfwWindow::whenWindowResized(WindowResizedCallback::Event on_resized) noexcept -> WindowResizedCallback::Handle {
        return m_when_window_resized.add(std::move(on_resized));
    }

    auto GlfwWindow::whenWindowScaled(WindowScaledCallback::Event on_scaled) noexcept -> WindowScaledCallback::Handle {
        return m_when_window_scaled.add(std::move(on_scaled));
    }
}
