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
