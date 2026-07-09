module;
#include "pP/Macros.h"
export module engine.app:window.handle;

import engine.core;
import engine.math;
import std;
import :input.listener;

export namespace pP {
    class Window;

    using SharedWindow = safe_ptr<const Window>;

    using WindowHandle = Numeric<void *, Window>;

    // ------------------------------------------------------------------
    // window properties definition
    // ------------------------------------------------------------------

    struct WindowModel {
        std::string m_title{};

        int2 framebuffer_size{};
        int2 window_position{};
        int2 window_size{};

        const bool m_decorated: 1 {true};
        const bool m_resizable: 1 {true};
        const bool m_visible: 1 {true};
    };

    // ------------------------------------------------------------------
    // callback to catch window events
    // ------------------------------------------------------------------

    template<typename... ArgsT>
    using WindowCallback = Callback<std::conditional_t<
        (std::is_void_v<std::remove_cvref_t<ArgsT> > && ...),
        void(const Window &window),
        void(const Window &window, ArgsT... args)
    > >;

    // ------------------------------------------------------------------
    // actual window instance
    // ------------------------------------------------------------------

    class Window : public WindowModel, public safe_object {
    public:
        WindowHandle m_handle{};

        WindowCallback<void> m_when_closed{};
        WindowCallback<bool> m_when_focused{};
        WindowCallback<bool> m_when_iconified{};
        WindowCallback<int2> m_when_moved{};
        WindowCallback<int2> m_when_resized{};

        InputListener m_inputs{};

        Window(WindowHandle handle, WindowModel &&model) noexcept;

        Window(const Window &) = delete;

        Window &operator=(const Window &) = delete;

        Window &operator=(Window &&) = delete;

        Window(Window &&other) noexcept;

#if PPR_ENABLE_ASSERTIONS
        ~Window() noexcept;
#endif

        [[nodiscard]] WindowHandle release() noexcept;
    };
}
