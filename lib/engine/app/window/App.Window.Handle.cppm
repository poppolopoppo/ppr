module;
#include "pP/Macros.h"
export module engine.app:window.handle;

import engine.core;
import engine.math;
import std;

export namespace pP {
    enum class EKeyboardKey : u8;
    enum class EMouseButton : u8;

    class Monitor;
    class Window;

    using SharedMonitor = safe_ptr<const Monitor>;
    using SharedWindow = safe_ptr<const Window>;
    using WindowHandle = Numeric<void *, Window>;

    using NativeWindowHandle = Numeric<void *, WindowHandle>;

    // ------------------------------------------------------------------
    // window properties definition
    // ------------------------------------------------------------------

    struct WindowModel {
        std::string m_title{};

        SharedMonitor m_fullscreen_monitor{};
        SharedWindow m_share_resources_with{};

        int2 m_window_position{};
        int2 m_window_size{};

        bool m_decorated: 1 {true};
        bool m_focused: 1 {true};
        bool m_iconified: 1 {false};
        bool m_resizable: 1 {true};
        bool m_visible: 1 {true};
    };

    // ------------------------------------------------------------------
    // actual window instance
    // ------------------------------------------------------------------

    template<typename... ArgsT>
    using WindowDelegate = Delegate<void(const Window &window, ArgsT...)>;

    class Window : public WindowModel, public safe_object {
    public:
        /// opaque window handle from platform abstraction
        WindowHandle m_handle{};
        /// native window handle from operating system
        NativeWindowHandle m_native{};

        float2 m_content_scale{1.0};
        int2 m_framebuffer_size{};
        bool m_hovered{false};

        WindowDelegate<> m_when_closed{};

        WindowDelegate<bool> m_when_focused{};
        WindowDelegate<bool> m_when_hovered{};
        WindowDelegate<bool> m_when_iconified{};

        WindowDelegate<const int2 &> m_when_moved{};
        WindowDelegate<const int2 &> m_when_resized{};
        WindowDelegate<const float2 &> m_when_scaled{};

        WindowDelegate<hal::native::char_t> m_when_character_input{};
        WindowDelegate<EKeyboardKey, bool> m_when_keyboard_pressed{};
        WindowDelegate<EKeyboardKey> m_when_keyboard_repeated{};

        WindowDelegate<EMouseButton, bool> m_when_mouse_clicked{};
        WindowDelegate<const float2 &> m_when_mouse_moved{};
        WindowDelegate<const float2 &> m_when_mouse_scrolled{};

        WindowDelegate<std::span<const char *> > m_when_drag_and_dropped{};

        Window(WindowHandle handle, NativeWindowHandle native, WindowModel &&model) noexcept;

        Window(const Window &) = delete;

        Window &operator=(const Window &) = delete;

        Window(Window &&other) noexcept;

        Window &operator=(Window &&) noexcept = delete;

#if PPR_ENABLE_ASSERTIONS
        ~Window() noexcept;
#endif

        [[nodiscard]] WindowHandle release() noexcept;
    };

    extern template class std23::function_ref<void (const Window &)>;
    extern template class std23::function_ref<void (const Window &, bool)>;
    extern template class std23::function_ref<void (const Window &, const float2 &)>;
    extern template class std23::function_ref<void (const Window &, EKeyboardKey)>;
    extern template class std23::function_ref<void (const Window &, EKeyboardKey, bool)>;
    extern template class std23::function_ref<void (const Window &, EMouseButton, bool)>;
    extern template class std23::function_ref<void (const Window &, hal::native::char_t)>;
}
