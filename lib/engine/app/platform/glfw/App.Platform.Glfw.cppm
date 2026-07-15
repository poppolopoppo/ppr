module;

export module engine.app:platform.glfw;

import :platform;
import :platform.glfw.input;
import :platform.glfw.window;
import :service.player;

export namespace pP {
    // ------------------------------------------------------------------
    // GLFW platform integration
    // ------------------------------------------------------------------

    class GlfwPlatform : public IPlatform {
    public:
        [[nodiscard]] safe_ptr<IInputService> getInputService() const noexcept override;

        [[nodiscard]] std::string_view getPlatformName() const noexcept override;

        [[nodiscard]] PlatformVersion getPlatformVersion() const noexcept override;

        [[nodiscard]] safe_ptr<IWindowService> getWindowService() const noexcept override;

        [[nodiscard]] safe_ptr<IPlayerService> getPlayerService() const noexcept override;

        void initializePlatform(Application &app) override;

        void shutdownPlatform(Application &app) override;

    private:
        GlfwWindow *m_window_service{};
        bool m_glfw_initialized{false};
    };
}
