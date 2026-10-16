module;
#include "pP/Macros.h"
module engine.app;

import :window.handle;

import engine.core;
import std;

namespace pP {
    // ------------------------------------------------------------------
    // window handle
    // ------------------------------------------------------------------

    Window::Window(const WindowHandle handle, NativeWindowHandle native, WindowModel &&model, void *display) noexcept
        : WindowModel(std::move(model)), m_handle{handle}, m_native(native), m_display{display} {
        PPR_ASSERT(nullptr != m_handle && "invalid window handle");
        PPR_ASSERT(nullptr != m_native && "invalid native handle");
    }

    Window::Window(Window &&other) noexcept : WindowModel(std::move(other)), m_handle(std::move(other.m_handle)) {
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
}
