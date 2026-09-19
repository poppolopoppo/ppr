module;

export module engine.app:platform.glfw;

import :platform;

import engine.core;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // GLFW platform integration
    // ------------------------------------------------------------------

    class GlfwInput;
    class GlfwPlayer;
    class GlfwWindow;

    class GlfwPlatform : public IPlatform {
    public:
        GlfwPlatform() noexcept;

        GlfwPlatform(const GlfwPlatform &) = delete;
        GlfwPlatform &operator =(const GlfwPlatform &) = delete;

        [[nodiscard]] std::error_code initialize(Application &app) override;

        [[nodiscard]] std::error_code shutdown(Application &app) override;

        [[nodiscard]] std::string_view getPlatformName() const noexcept override;

        [[nodiscard]] platform::Version getPlatformVersion() const noexcept override;

        [[nodiscard]] safe_ptr<Application> getApplication() const noexcept override;

        [[nodiscard]] safe_ptr<IInputService> getInputService() const noexcept override;

        [[nodiscard]] safe_ptr<IWindowService> getWindowService() const noexcept override;

        [[nodiscard]] safe_ptr<IPlayerService> getPlayerService() const noexcept override;

        [[nodiscard]] std::error_code update(TimeSpan dt) override;

    private:
        safe_ptr<Application> m_application{};

        std::unique_ptr<GlfwInput> m_input_service{};
        std::unique_ptr<GlfwPlayer> m_player_service{};
        std::unique_ptr<GlfwWindow> m_window_service{};
    };
}
