module;
#include "pP/Macros.h"
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

export module engine.platform:glfw;
import :platform;
import :window;
import std;

export namespace pP::detail {

class GlfwWindow : public IWindow {
    GLFWwindow* m_handle{};
    CursorMode m_cursorMode = CursorMode::Normal;
public:
    explicit GlfwWindow(GLFWwindow* handle) noexcept : m_handle(handle) {}
    ~GlfwWindow() noexcept override;

    GlfwWindow(const GlfwWindow&) = delete;
    GlfwWindow& operator=(const GlfwWindow&) = delete;

    [[nodiscard]] bool shouldClose() const noexcept override;
    void swapBuffers() noexcept override;
    void setTitle(std::string_view title) override;
    void setCursorMode(CursorMode mode) override;
    [[nodiscard]] CursorMode getCursorMode() const noexcept override;
    [[nodiscard]] void* getNativeHandle() const noexcept override;
};

class GlfwPlatform : public IPlatform {
    bool m_initialized = false;
public:
    GlfwPlatform() noexcept = default;
    ~GlfwPlatform() noexcept override;

    GlfwPlatform(const GlfwPlatform&) = delete;
    GlfwPlatform& operator=(const GlfwPlatform&) = delete;

    [[nodiscard]] bool initialize();
    [[nodiscard]] std::expected<std::unique_ptr<IWindow>, int> createWindow(const WindowDesc& desc) override;
    void processEvents() override;
    [[nodiscard]] std::span<const char* const> getRequiredInstanceExtensions() const override;
    [[nodiscard]] double getTime() const noexcept override;
};

} // namespace pP::detail
