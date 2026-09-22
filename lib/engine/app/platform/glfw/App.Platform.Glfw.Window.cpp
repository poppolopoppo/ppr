module;

#include "pP/Macros.h"
#include "App.Platform.Glfw.include.hpp"

module engine.app;

import :platform.glfw.window;
import :window.handle;
import :window.monitor;
import :window.viewport;

import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(GlfwWindow, info, none);

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

        return monitor;
    }

    // ------------------------------------------------------------------
    // GLFW window service init/destroy
    // ------------------------------------------------------------------

    static safe_ptr<GlfwWindow> g_glfw_window{};

    std::error_code GlfwWindow::initialize(const Application &app) {
        g_glfw_window.reset(this);

        PPR_RETURN_ERROR_ON_FAIL(GlfwWindow, initializeMonitors_());

        m_application.reset(std::addressof(app));
        return default_value_v;
    }

    std::error_code GlfwWindow::shutdown() {
        PPR_DEFER {
            m_main_viewport.reset();
            m_focused_window.reset();
            m_main_window.reset();
            m_primary_monitor.reset();

            m_windows.clear();
            m_monitors.clear();
        };

        ::glfwSetMonitorCallback(nullptr);

        m_application.reset();
        g_glfw_window.reset();

        std::error_code first_err{};

        for (const std::unique_ptr<Window> &window: m_windows) {
            PPR_RETAIN_ERROR_ON_FAIL(GlfwWindow, first_err, m_when_window_destroyed(*window));

            ::glfwDestroyWindow(glfwHandle_(window->release()));
        }

        for (const std::unique_ptr<Monitor> &monitor: m_monitors) {
            PPR_RETAIN_ERROR_ON_FAIL(GlfwWindow, first_err, m_when_monitor_disconnected(*monitor));

            ::glfwSetMonitorUserPointer(glfwHandle_(monitor->release()), nullptr);
        }

        return first_err;
    }

    static void glfwMonitorCallback_(GLFWmonitor *p_glfw_monitor, const int status) {
        PPR_ASSERT(p_glfw_monitor != nullptr);
        GlfwWindow &service = *g_glfw_window;

        if (status == GLFW_CONNECTED) {
            std::unique_ptr<Monitor> monitor = glfwCreateMonitor_(p_glfw_monitor);
            service.m_monitors.push_back(std::move(monitor));
            Monitor &published_monitor = *service.m_monitors.back();
            ::glfwSetMonitorUserPointer(p_glfw_monitor, &published_monitor);

            if (published_monitor.m_primary_monitor) {
                service.m_primary_monitor.reset(&published_monitor);
            }

            PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_monitor_connected(published_monitor));
            return;
        }

        if (status == GLFW_DISCONNECTED) {
            if (const auto it = glfwAllocation_(service.m_monitors, MonitorHandle{p_glfw_monitor});
                service.m_monitors.end() != it) {
                PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_monitor_disconnected(**it));
                service.m_monitors.erase(it);
            }
            return;
        }

        std::unreachable();
    }

    std::error_code GlfwWindow::initializeMonitors_() {
        int monitors_count{0};
        GLFWmonitor **const p_monitors_arr = ::glfwGetMonitors(&monitors_count);
        if (p_monitors_arr == nullptr || monitors_count == 0) {
            return make_error_code(std::errc::no_such_device);
        }

        Array<std::unique_ptr<Monitor> > staged_monitors{};
        staged_monitors.reserve(safe_narrowing(monitors_count));

        safe_ptr<const Monitor> staged_primary{};

        for (int i = 0; i < monitors_count; ++i) {
            if (PPR_ENSURE(p_monitors_arr[i])) {
                std::unique_ptr<Monitor> monitor = glfwCreateMonitor_(p_monitors_arr[i]);

                if (monitor->m_primary_monitor) {
                    staged_primary.reset(monitor.get());
                }

                staged_monitors.push_back(std::move(monitor));
            }
        }

        m_monitors = std::move(staged_monitors);
        m_primary_monitor = staged_primary;

        std::ranges::for_each(m_monitors, [](const std::unique_ptr<Monitor> &monitor) noexcept {
            ::glfwSetMonitorUserPointer(glfwHandle_(monitor->m_handle), monitor.get());
        });

        ::glfwSetMonitorCallback(&glfwMonitorCallback_);
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // GLFW window events
    // ------------------------------------------------------------------

    std::error_code GlfwWindow::pollEvents() {
        ::glfwPollEvents();
        return default_value_v;
    }

    std::error_code GlfwWindow::waitEvents() {
        ::glfwWaitEvents();
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // GLFW monitors
    // ------------------------------------------------------------------

    std::error_code GlfwWindow::enumerateMonitors(const Collector<SharedMonitor> each_monitor) const noexcept {
        for (const std::unique_ptr<Monitor> &monitor: m_monitors) {
            if (const std::error_code err = each_monitor(SharedMonitor(monitor.get()))) [[unlikely]] {
                return err;
            }
        }
        return default_value_v;
    }

    [[nodiscard]] SharedMonitor GlfwWindow::getPrimaryMonitor() const noexcept {
        return m_primary_monitor;
    }

    std::error_code GlfwWindow::enumerateMonitorVideoModes(const Monitor &monitor, const Collector<VideoMode> each_video_mode) const noexcept {
        int video_modes_count{0};
        const ::GLFWvidmode *const p_video_modes_arr = ::glfwGetVideoModes(glfwHandle_(monitor.m_handle), &video_modes_count);
        PPR_ASSERT(p_video_modes_arr != nullptr || video_modes_count == 0);
        if (p_video_modes_arr == nullptr) {
            return make_error_code(std::errc::no_such_device);
        }

        for (int i = 0; i < video_modes_count; ++i) {
            if (const std::error_code err = each_video_mode(glfwVideoMode_(p_video_modes_arr[i]))) [[unlikely]] {
                return err;
            }
        }

        return default_value_v;
    }

    void GlfwWindow::setMonitorGamma(const Monitor &monitor, const float gamma) noexcept {
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
    // GLFW window
    // ------------------------------------------------------------------

    [[nodiscard]] static Window &getWindowFromGlfwHandle_(::GLFWwindow *p_glfw_window) noexcept {
        PPR_ASSERT(p_glfw_window != nullptr);
        return *static_cast<Window *>(::glfwGetWindowUserPointer(p_glfw_window));
    }

    /// -> window state events:

    static void glfwWindowCloseCallback_(::GLFWwindow *p_glfw_window) {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        window.m_visible = false;
        window.m_when_closed(window);

        GlfwWindow &service = *g_glfw_window;
        PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_window_closed(window));

        if (service.m_focused_window == &window) {
            service.m_focused_window.reset();
        }

        if (service.m_main_window == &window) {
            service.m_main_window.reset();

            // closing the main window also closes the application:
            if (service.m_application.isValid()) {
                service.m_application->requestExit(default_value_v);
            }
        }
    }

    static void glfwWindowFocusCallback_(::GLFWwindow *p_glfw_window, const int focused) {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const bool is_focused = focused == GLFW_TRUE;
        window.m_focused = is_focused;
        window.m_when_focused(window, is_focused);

        GlfwWindow &service = *g_glfw_window;
        PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_window_focused(window, window.m_focused));

        if (window.m_focused) {
            service.m_focused_window = safe_ptr(&window);
        } else if (service.m_focused_window == &window) {
            service.m_focused_window.reset();
        }
    }

    static void glfwWindowIconifyCallback_(::GLFWwindow *p_glfw_window, const int iconified) {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const bool is_iconified = iconified == GLFW_TRUE;
        window.m_iconified = is_iconified;
        window.m_when_iconified(window, is_iconified);

        GlfwWindow &service = *g_glfw_window;
        PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_window_iconified(window, window.m_iconified));
    }

    /// -> window geometry events:

    static void glfwWindowPosCallback_(::GLFWwindow *p_glfw_window, const int position_x, const int position_y) {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const int2 old_position = window.m_window_position;
        window.m_window_position = int2{position_x, position_y};
        window.m_when_moved(window, old_position);

        GlfwWindow &service = *g_glfw_window;
        PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_window_moved(window, old_position));
    }

    static void glfwWindowSizeCallback_(::GLFWwindow *p_glfw_window, const int size_x, const int size_y) {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        [[maybe_unused]] const int2 old_size = window.m_window_size;
        window.m_window_size = int2{size_x, size_y};

#if 0 // The framebuffer callback below is the single driver of m_when_resized: see comment bellow
        window.m_when_resized(window, old_size);

        auto &g_service = GlfwWindow::get();
        PPR_LOG_WARNING_ON_FAIL(GlfwWindow, g_service.m_when_window_resized(window, old_size));
#endif
    }

    // The framebuffer callback below is the single driver of m_when_resized: rendering and
    // ImGui are sized from m_framebuffer_size, and CallbackSink stores only the first deferred
    // event per poll cycle, so notifying from both callbacks would drop the framebuffer one.
    static void glfwFramebufferSizeCallback_(::GLFWwindow *p_glfw_window, const int size_x, const int size_y) {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const int2 old_size = window.m_framebuffer_size;
        window.m_framebuffer_size = int2{size_x, size_y};
        window.m_when_resized(window, old_size);

        GlfwWindow &service = *g_glfw_window;
        PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_window_resized(window, old_size));
    }

    static void glfwWindowContentScaleCallback_(::GLFWwindow *p_glfw_window, const float scale_x, const float scale_y) {
        Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const float2 old_content_scale = window.m_content_scale;
        window.m_content_scale = float2{scale_x, scale_y};
        window.m_when_scaled(window, old_content_scale);

        GlfwWindow &service = *g_glfw_window;
        PPR_LOG_WARNING_ON_FAIL(GlfwWindow, service.m_when_window_scaled(window, old_content_scale));
    }

    /// -> window input events:

    [[nodiscard]] static std::optional<EKeyboardKey> glfwToKeyboardKey_(const int key) noexcept {
        if (key >= GLFW_KEY_SPACE && key <= 126) {
            if (key == GLFW_KEY_SPACE) {
                return EKeyboardKey::space;
            }
            if (key >= 'A' && key <= 'Z') {
                return static_cast<EKeyboardKey>(key + 32);
            }
            return static_cast<EKeyboardKey>(key);
        }

        switch (key) {
            case GLFW_KEY_ESCAPE: return EKeyboardKey::escape;
            case GLFW_KEY_ENTER: return EKeyboardKey::enter;
            case GLFW_KEY_TAB: return EKeyboardKey::tab;
            case GLFW_KEY_BACKSPACE: return EKeyboardKey::backspace;
            case GLFW_KEY_INSERT: return EKeyboardKey::insert;
            case GLFW_KEY_DELETE: return EKeyboardKey::delete_;
            case GLFW_KEY_RIGHT: return EKeyboardKey::right_arrow;
            case GLFW_KEY_LEFT: return EKeyboardKey::left_arrow;
            case GLFW_KEY_DOWN: return EKeyboardKey::down_arrow;
            case GLFW_KEY_UP: return EKeyboardKey::up_arrow;
            case GLFW_KEY_PAGE_UP: return EKeyboardKey::page_up;
            case GLFW_KEY_PAGE_DOWN: return EKeyboardKey::page_down;
            case GLFW_KEY_HOME: return EKeyboardKey::home;
            case GLFW_KEY_END: return EKeyboardKey::end;
            case GLFW_KEY_CAPS_LOCK: return EKeyboardKey::caps_lock;
            case GLFW_KEY_NUM_LOCK: return EKeyboardKey::num_lock;
            case GLFW_KEY_SCROLL_LOCK: return EKeyboardKey::scroll_lock;
            case GLFW_KEY_PAUSE: return EKeyboardKey::pause;
            case GLFW_KEY_PRINT_SCREEN: return EKeyboardKey::print_screen;
            case GLFW_KEY_LEFT_SHIFT: return EKeyboardKey::left_shift;
            case GLFW_KEY_RIGHT_SHIFT: return EKeyboardKey::right_shift;
            case GLFW_KEY_LEFT_CONTROL: return EKeyboardKey::left_control;
            case GLFW_KEY_RIGHT_CONTROL: return EKeyboardKey::right_control;
            case GLFW_KEY_LEFT_ALT: return EKeyboardKey::left_alt;
            case GLFW_KEY_RIGHT_ALT: return EKeyboardKey::right_alt;
            case GLFW_KEY_LEFT_SUPER: return EKeyboardKey::left_super;
            case GLFW_KEY_RIGHT_SUPER: return EKeyboardKey::right_super;
            case GLFW_KEY_F1: return EKeyboardKey::f1;
            case GLFW_KEY_F2: return EKeyboardKey::f2;
            case GLFW_KEY_F3: return EKeyboardKey::f3;
            case GLFW_KEY_F4: return EKeyboardKey::f4;
            case GLFW_KEY_F5: return EKeyboardKey::f5;
            case GLFW_KEY_F6: return EKeyboardKey::f6;
            case GLFW_KEY_F7: return EKeyboardKey::f7;
            case GLFW_KEY_F8: return EKeyboardKey::f8;
            case GLFW_KEY_F9: return EKeyboardKey::f9;
            case GLFW_KEY_F10: return EKeyboardKey::f10;
            case GLFW_KEY_F11: return EKeyboardKey::f11;
            case GLFW_KEY_F12: return EKeyboardKey::f12;
            default: return std::nullopt;
        }
    }

    static void glfwKeyCallback_(::GLFWwindow *const p_glfw_window, const int glfw_key, [[maybe_unused]] const int scancode, const int action,
                                 [[maybe_unused]] const int mods) {
        const std::optional<EKeyboardKey> key = glfwToKeyboardKey_(glfw_key);
        if (not key.has_value()) {
            return;
        }

        const Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        switch (action) {
            case GLFW_PRESS:
                window.m_when_keyboard_pressed(window, *key, true);
                break;
            case GLFW_RELEASE:
                window.m_when_keyboard_pressed(window, *key, false);
                break;
            case GLFW_REPEAT:
                window.m_when_keyboard_repeated(window, *key);
                break;
            default:
                PPR_ASSERT(false && "unhandled GLFW keyboard action");
                break;
        }
    }

    static void glfwCharCallback_(::GLFWwindow *const p_glfw_window, const unsigned int codepoint) {
        if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
            return;
        }

        const Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        window.m_when_character_input(window, checked_cast<hal::native::char_t>(codepoint));
    }

    static void glfwCursorEnterCallback_(::GLFWwindow *p_glfw_window, const int entered) {
        const Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        window.m_when_hovered(window, entered);
    }

    static void glfwMouseButtonCallback_(::GLFWwindow *p_glfw_window, const int glfw_button, const int action, [[maybe_unused]] const int mods) {
        if (glfw_button < 0 || glfw_button > 4) {
            return;
        }

        const auto button = static_cast<EMouseButton>(glfw_button);

        const Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        switch (action) {
            case GLFW_PRESS:
                window.m_when_mouse_clicked(window, button, true);
                break;
            case GLFW_RELEASE:
                window.m_when_mouse_clicked(window, button, false);
                break;
            default:
                PPR_ASSERT(false && "unhandled GLFW mouse action");
                break;
        }
    }

    static void glfwCursorPosCallback_(::GLFWwindow *p_glfw_window, const double cursor_x, const double cursor_y) {
        const Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const float2 new_cursor_pos{static_cast<float>(cursor_x), static_cast<float>(cursor_y)};
        window.m_when_mouse_moved(window, new_cursor_pos);
    }

    static void glfwScrollCallback_(::GLFWwindow *p_glfw_window, const double offset_x, const double offset_y) {
        const Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        const float2 add_wheel_delta{static_cast<float>(offset_x), static_cast<float>(offset_y)};
        window.m_when_mouse_scrolled(window, add_wheel_delta);
    }

    /// -> window drag & drop:

    static void glfwDropCallback_(GLFWwindow *p_glfw_window, const int path_count, const char *paths[]) {
        const Window &window = getWindowFromGlfwHandle_(p_glfw_window);
        window.m_when_drag_and_dropped(window, std::span{paths, paths + path_count});
    }

    /// -> window handling:

    static void *glfwGetNativeWindowHandle(GLFWwindow *p_glfw_window) {
        if (p_glfw_window != nullptr) [[likely]] {
#ifdef _WIN32
            return ::glfwGetWin32Window(p_glfw_window);
#elif defined(__APPLE__)
            return (void*)::glfwGetCocoaWindow(p_glfw_window);
#else
            return nullptr;
#endif
        }
        return nullptr;
    }

    std::error_code GlfwWindow::createWindow(
        WindowModel &&definition,
        safe_ptr<Window> *window_write_ref) {
        PPR_ASSERT(window_write_ref);

        ::glfwWindowHint(GLFW_FOCUSED, definition.m_focused ? GLFW_TRUE : GLFW_FALSE);
        ::glfwWindowHint(GLFW_VISIBLE, definition.m_visible ? GLFW_TRUE : GLFW_FALSE);
        ::glfwWindowHint(GLFW_DECORATED, definition.m_decorated ? GLFW_TRUE : GLFW_FALSE);
        ::glfwWindowHint(GLFW_RESIZABLE, definition.m_resizable ? GLFW_TRUE : GLFW_FALSE);

        auto p_glfw_window = std::unique_ptr<GLFWwindow, decltype(&::glfwDestroyWindow)>{
            ::glfwCreateWindow(
                definition.m_window_size.x,
                definition.m_window_size.y,
                definition.m_title.data(),
                definition.m_fullscreen_monitor.isValid() ? glfwHandle_(definition.m_fullscreen_monitor->m_handle) : nullptr,
                definition.m_share_resources_with.isValid() ? glfwHandle_(definition.m_share_resources_with->m_handle) : nullptr),
            &::glfwDestroyWindow
        };

        if (not p_glfw_window) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        ::glfwSetWindowAttrib(p_glfw_window.get(), GLFW_DECORATED, definition.m_decorated);
        ::glfwSetWindowAttrib(p_glfw_window.get(), GLFW_RESIZABLE, definition.m_resizable);

        int2 framebuffer_size{};
        ::glfwGetFramebufferSize(p_glfw_window.get(), &framebuffer_size.x, &framebuffer_size.y);

        int2 window_position{};
        ::glfwGetWindowPos(p_glfw_window.get(), &window_position.x, &window_position.y);

        int2 window_size{};
        ::glfwGetWindowSize(p_glfw_window.get(), &window_size.x, &window_size.y);

        float2 content_scale{1.0};
        ::glfwGetWindowContentScale(p_glfw_window.get(), &content_scale.x, &content_scale.y);

        ::glfwSetWindowCloseCallback(p_glfw_window.get(), &glfwWindowCloseCallback_);
        ::glfwSetWindowFocusCallback(p_glfw_window.get(), &glfwWindowFocusCallback_);
        ::glfwSetWindowIconifyCallback(p_glfw_window.get(), &glfwWindowIconifyCallback_);

        ::glfwSetWindowPosCallback(p_glfw_window.get(), &glfwWindowPosCallback_);
        ::glfwSetWindowSizeCallback(p_glfw_window.get(), &glfwWindowSizeCallback_);
        ::glfwSetFramebufferSizeCallback(p_glfw_window.get(), &glfwFramebufferSizeCallback_);
        ::glfwSetWindowContentScaleCallback(p_glfw_window.get(), &glfwWindowContentScaleCallback_);

        ::glfwSetKeyCallback(p_glfw_window.get(), &glfwKeyCallback_);
        ::glfwSetCharCallback(p_glfw_window.get(), &glfwCharCallback_);

        ::glfwSetMouseButtonCallback(p_glfw_window.get(), &glfwMouseButtonCallback_);
        ::glfwSetCursorPosCallback(p_glfw_window.get(), &glfwCursorPosCallback_);
        ::glfwSetCursorEnterCallback(p_glfw_window.get(), &glfwCursorEnterCallback_);
        ::glfwSetScrollCallback(p_glfw_window.get(), &glfwScrollCallback_);

        ::glfwSetDropCallback(p_glfw_window.get(), &glfwDropCallback_);

        const NativeWindowHandle native_handle{glfwGetNativeWindowHandle(p_glfw_window.get())};

        auto window = std::make_unique<Window>(
            WindowHandle{p_glfw_window.release()},
            native_handle,
            std::move(definition));
        window->m_content_scale = content_scale;
        window->m_framebuffer_size = framebuffer_size;
        window->m_window_position = window_position;
        window->m_window_size = window_size;

        ::glfwSetWindowUserPointer(glfwHandle_(window->m_handle), window.get());

        window_write_ref->reset(window.get());
        m_windows.push_back(std::move(window));

        if (const std::error_code err = m_when_window_created(*window)) [[unlikely]] {
            std::ignore = destroyWindow(*window_write_ref);
            return err;
        }
        return default_value_v;
    }

    std::error_code GlfwWindow::destroyWindow(SharedWindow &&window) {
        const auto it = std::ranges::find_if(
            m_windows,
            [&](const std::unique_ptr<Window> &owned) noexcept -> bool {
                return owned.get() == &*window;
            });

        if (PPR_ENSURE(m_windows.end() != it)) {
            const bool was_main = (m_main_window == window);
            const bool was_focused = (m_focused_window == window);

            const auto extracted = std::move(*it);
            m_windows.erase(it);
            window = nullptr;

            PPR_DEFER {
                if (was_main) {
                    m_main_window.reset();
                }

                if (was_focused) {
                    m_focused_window.reset();
                }

                ::glfwDestroyWindow(glfwHandle_(extracted->release()));
            };

            return m_when_window_destroyed(*extracted);
        }
        return default_value_v;
    }

    SharedWindow GlfwWindow::setMainWindow(SharedWindow window) {
        PPR_ASSERT(not window.isValid() or glfwAllocation_(m_windows, *window) != m_windows.end());

        SharedWindow old_main_window{m_main_window};
        m_main_window = std::move(window);

        if (m_main_window.isValid()) {

        }

        return old_main_window;
    }

    [[nodiscard]] SharedMonitor GlfwWindow::getWindowFullscreenMonitor(const Window &window) const noexcept {
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

    void GlfwWindow::setWindowShouldClose(const Window &window, const bool value) {
        ::glfwSetWindowShouldClose(glfwHandle_(window.m_handle), value);
    }

    void GlfwWindow::setWindowCursorMode(const Window &window, const ECursorMode mode) {
        ::GLFWwindow *const p_glfw_window = glfwHandle_(window.m_handle);

        switch (mode) {
            case ECursorMode::normal:
                ::glfwSetInputMode(p_glfw_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                break;
            case ECursorMode::captured:
                ::glfwSetInputMode(p_glfw_window, GLFW_CURSOR, GLFW_CURSOR_CAPTURED);
                break;
            case ECursorMode::disabled:
                ::glfwSetInputMode(p_glfw_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                break;
            case ECursorMode::hidden:
                ::glfwSetInputMode(p_glfw_window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
                break;
        }
    }

    // ------------------------------------------------------------------
    // GLFW window manipulation
    // ------------------------------------------------------------------

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

    void GlfwWindow::focusWindow(const Window &window) {
        ::glfwFocusWindow(glfwHandle_(window.m_handle));
    }

    void GlfwWindow::swapWindowBuffers(const Window &window) {
        ::glfwSwapBuffers(glfwHandle_(window.m_handle));
    }

    void *GlfwWindow::getWindowNativeHandle(const Window &window) const noexcept {
        return glfwGetNativeWindowHandle(glfwHandle_(window.m_handle));
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
