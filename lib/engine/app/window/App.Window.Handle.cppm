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

        int2 m_window_position{};
        int2 m_window_size{};

        bool m_decorated{true};
        bool m_focused{true};
        bool m_iconified{false};
        bool m_resizable{true};
        bool m_visible{true};
    };

    // ------------------------------------------------------------------
    // callback to catch window events
    // ------------------------------------------------------------------

    template<typename... ArgsT>
    using WindowCallback = Callback<void(const Window &window, ArgsT...)>;

    // ------------------------------------------------------------------
    // actual window instance
    // ------------------------------------------------------------------

    class Window : public WindowModel, public safe_object {
    public:
        WindowHandle m_handle{};

        int2 m_framebuffer_size{};
        float2 m_content_scale{1.0};

        WindowCallback<> m_when_closed{};
        WindowCallback<bool> m_when_focused{};
        WindowCallback<bool> m_when_iconified{};
        WindowCallback<int2> m_when_moved{};
        WindowCallback<int2> m_when_resized{};
        WindowCallback<float2> m_when_scaled{};

        Window(WindowHandle handle, WindowModel &&model) noexcept;

        Window(const Window &) = delete;

        Window &operator=(const Window &) = delete;

        Window(Window &&other) noexcept;

        Window &operator=(Window &&) noexcept = delete;

#if PPR_ENABLE_ASSERTIONS
        ~Window() noexcept;
#endif

        [[nodiscard]] WindowHandle release() noexcept;
    };
}
