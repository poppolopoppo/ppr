module;
#include "pP/Macros.h"
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

module engine.platform;
import engine.core;
import :glfw;
import std;

namespace pP::detail {

GlfwWindow::~GlfwWindow() noexcept {
    if (m_handle) {
        glfwDestroyWindow(m_handle);
        m_handle = nullptr;
    }
}

bool GlfwWindow::shouldClose() const noexcept {
    return glfwWindowShouldClose(m_handle) != 0;
}

void GlfwWindow::swapBuffers() noexcept {
    glfwSwapBuffers(m_handle);
}

void GlfwWindow::setTitle(std::string_view title) {
    glfwSetWindowTitle(m_handle, title.data());
}

void GlfwWindow::setCursorMode(CursorMode mode) {
    m_cursorMode = mode;
    int glfwMode;
    switch (mode) {
        case CursorMode::Normal:   glfwMode = GLFW_CURSOR_NORMAL;   break;
        case CursorMode::Hidden:   glfwMode = GLFW_CURSOR_HIDDEN;   break;
        case CursorMode::Disabled: glfwMode = GLFW_CURSOR_DISABLED; break;
    }
    glfwSetInputMode(m_handle, GLFW_CURSOR, glfwMode);
}

CursorMode GlfwWindow::getCursorMode() const noexcept {
    return m_cursorMode;
}

void* GlfwWindow::getNativeHandle() const noexcept {
#ifdef _WIN32
    return reinterpret_cast<void*>(glfwGetWin32Window(m_handle));
#else
    return reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(m_handle)));
#endif
}

bool GlfwPlatform::initialize() {
    if (m_initialized) return true;
    if (!glfwInit()) return false;
    m_initialized = true;
    return true;
}

void GlfwPlatform::setInputCallbacks(const GlfwKeyCallback keyCb, const GlfwMouseCallback mouseCb, void* const context) {
    m_keyCb = keyCb;
    m_mouseCb = mouseCb;
    m_inputContext = context;
}

GlfwPlatform::~GlfwPlatform() noexcept {
    if (m_initialized) {
        glfwTerminate();
        m_initialized = false;
    }
}

std::expected<std::unique_ptr<IWindow>, int> GlfwPlatform::createWindow(const WindowDesc& desc) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    GLFWwindow* handle = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
    if (!handle) {
        return std::unexpected(-1);
    }

    glfwSetKeyCallback(handle, [](GLFWwindow* w, int key, int, int action, int) {
        auto* platform = static_cast<GlfwPlatform*>(glfwGetWindowUserPointer(w));
        if (platform && platform->m_keyCb) {
            platform->m_keyCb(key, action != GLFW_RELEASE, platform->m_inputContext);
        }
    });
    glfwSetMouseButtonCallback(handle, [](GLFWwindow* w, int button, int action, int) {
        auto* platform = static_cast<GlfwPlatform*>(glfwGetWindowUserPointer(w));
        if (platform && platform->m_mouseCb) {
            platform->m_mouseCb(button, action != GLFW_RELEASE, platform->m_inputContext);
        }
    });
    glfwSetWindowUserPointer(handle, this);

    return std::unique_ptr<IWindow>(std::make_unique<GlfwWindow>(handle));
}

void GlfwPlatform::processEvents() {
    glfwPollEvents();
}

std::span<const char* const> GlfwPlatform::getRequiredInstanceExtensions() const {
    uint32_t count;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    return {extensions, count};
}

double GlfwPlatform::getTime() const noexcept {
    return glfwGetTime();
}

} // namespace pP::detail

namespace pP {

InputKey fromGlfwKey(int glfwKey) noexcept {
    using namespace keys;
    switch (glfwKey) {
        case GLFW_KEY_A: return a; case GLFW_KEY_B: return b;
        case GLFW_KEY_C: return c; case GLFW_KEY_D: return d;
        case GLFW_KEY_E: return e; case GLFW_KEY_F: return f;
        case GLFW_KEY_G: return g; case GLFW_KEY_H: return h;
        case GLFW_KEY_I: return i; case GLFW_KEY_J: return j;
        case GLFW_KEY_K: return k; case GLFW_KEY_L: return l;
        case GLFW_KEY_M: return m; case GLFW_KEY_N: return n;
        case GLFW_KEY_O: return o; case GLFW_KEY_P: return p;
        case GLFW_KEY_Q: return q; case GLFW_KEY_R: return r;
        case GLFW_KEY_S: return s; case GLFW_KEY_T: return t;
        case GLFW_KEY_U: return u; case GLFW_KEY_V: return v;
        case GLFW_KEY_W: return w; case GLFW_KEY_X: return x;
        case GLFW_KEY_Y: return y; case GLFW_KEY_Z: return z;
        case GLFW_KEY_0: return num0; case GLFW_KEY_1: return num1;
        case GLFW_KEY_2: return num2; case GLFW_KEY_3: return num3;
        case GLFW_KEY_4: return num4; case GLFW_KEY_5: return num5;
        case GLFW_KEY_6: return num6; case GLFW_KEY_7: return num7;
        case GLFW_KEY_8: return num8; case GLFW_KEY_9: return num9;
        case GLFW_KEY_SPACE: return space;
        case GLFW_KEY_ENTER: return enter;
        case GLFW_KEY_ESCAPE: return escape;
        case GLFW_KEY_TAB: return tab;
        case GLFW_KEY_BACKSPACE: return backspace;
        case GLFW_KEY_LEFT_SHIFT: return leftShift;
        case GLFW_KEY_RIGHT_SHIFT: return rightShift;
        case GLFW_KEY_LEFT_CONTROL: return leftCtrl;
        case GLFW_KEY_RIGHT_CONTROL: return rightCtrl;
        case GLFW_KEY_LEFT_ALT: return leftAlt;
        case GLFW_KEY_RIGHT_ALT: return rightAlt;
        default: return unknown;
    }
}

InputKey fromGlfwMouseButton(int glfwButton) noexcept {
    switch (glfwButton) {
        case GLFW_MOUSE_BUTTON_LEFT: return keys::mouseLeft;
        case GLFW_MOUSE_BUTTON_RIGHT: return keys::mouseRight;
        case GLFW_MOUSE_BUTTON_MIDDLE: return keys::mouseMiddle;
        default: return keys::unknown;
    }
}

} // namespace pP
