module;

export module engine.app:platform.glfw;

import :platform;
import :platform.glfw.input;
import :platform.glfw.player;
import :platform.glfw.window;
import :service.player;

export namespace pP {
    // ------------------------------------------------------------------
    // GLFW platform integration
    // ------------------------------------------------------------------

    class GlfwPlatform : public IPlatform {
    public:
        [[nodiscard]] std::error_code initialize(Application &app) override;

        [[nodiscard]] std::error_code shutdown(Application &app) override;

        [[nodiscard]] std::string_view getPlatformName() const noexcept override;

        [[nodiscard]] platform::Version getPlatformVersion() const noexcept override;

        [[nodiscard]] safe_ptr<IInputService> getInputService() const noexcept override;

        [[nodiscard]] safe_ptr<IWindowService> getWindowService() const noexcept override;

        [[nodiscard]] safe_ptr<IPlayerService> getPlayerService() const noexcept override;

    private:
        safe_ptr<GlfwWindow> m_window_service{};
        bool m_glfw_initialized{false};
    };
}
