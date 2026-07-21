module;
#include "pP/Macros.h"
module engine.app;

import :window.handle;

namespace pP {
    // ------------------------------------------------------------------
    // window handle
    // ------------------------------------------------------------------

    Window::Window(const WindowHandle handle, WindowModel &&model) noexcept
        : WindowModel(std::move(model)), m_handle{handle} {
        PPR_ASSERT(nullptr != m_handle && "invalid handle");
    }

    Window::Window(Window &&other) noexcept
        : WindowModel(std::move(other)),
          m_handle(std::move(other.m_handle)) {
        other.m_handle = default_value_v;
    }

#if PPR_ENABLE_ASSERTIONS
    Window::~Window() noexcept {
        PPR_ASSERT(m_handle == nullptr);
    }
#endif

    WindowHandle Window::release() noexcept {
        PPR_ASSERT(m_handle != nullptr);
        WindowHandle release{nullptr};
        swap(release, m_handle);
        return release;
    }

    std::error_code Window::update() {
        return make_error_code({
            m_when_closed.sink(),
            m_when_focused.sink(),
            m_when_iconified.sink(),
            m_when_moved.sink(),
            m_when_resized.sink(),
            m_when_scaled.sink()
        });
    }
}
