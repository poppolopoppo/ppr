module;
#include "pP/Macros.h"
export module engine.platform:null;
import :platform;
import :window;
import std;

export namespace pP::detail {

class NullWindow : public IWindow {
    bool m_shouldClose = false;
public:
    NullWindow() noexcept = default;
    ~NullWindow() noexcept override = default;

    [[nodiscard]] bool shouldClose() const noexcept override { return m_shouldClose; }
    void swapBuffers() noexcept override {}
    void setTitle(std::string_view) override {}
    void setCursorMode(CursorMode) override {}
    [[nodiscard]] CursorMode getCursorMode() const noexcept override { return CursorMode::Normal; }
    [[nodiscard]] void* getNativeHandle() const noexcept override { return nullptr; }

    void forceClose() noexcept { m_shouldClose = true; }
};

class NullPlatform : public IPlatform {
    std::vector<std::unique_ptr<NullWindow>> m_windows{};
    double m_time = 0.0;
public:
    NullPlatform() noexcept = default;
    ~NullPlatform() noexcept override;

    NullPlatform(const NullPlatform&) = delete;
    NullPlatform& operator=(const NullPlatform&) = delete;

    [[nodiscard]] std::expected<std::unique_ptr<IWindow>, int> createWindow(const WindowDesc& desc) override;
    void processEvents() override {}
    [[nodiscard]] std::span<const char* const> getRequiredInstanceExtensions() const override { return {}; }
    [[nodiscard]] double getTime() const noexcept override { return m_time; }
    void advanceTime(double dt) noexcept { m_time += dt; }
};

} // namespace pP::detail
