module;

export module engine.app:platform.glfw;

import :application;
import :platform;
import :platform.glfw.input;
import :platform.glfw.player;
import :platform.glfw.window;

import engine.core;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // GLFW platform integration
    // ------------------------------------------------------------------

    class GlfwPlatform : public IPlatform {
    public:
        GlfwPlatform() noexcept;

        GlfwPlatform(const GlfwPlatform &) = delete;
        GlfwPlatform &operator=(const GlfwPlatform &) = delete;

        [[nodiscard]] std::error_code initialize(Application &app) override;

        [[nodiscard]] std::error_code shutdown(Application &app) override;

        [[nodiscard]] std::string_view getPlatformName() const noexcept override;

        [[nodiscard]] platform::Version getPlatformVersion() const noexcept override;

        [[nodiscard]] safe_ptr<Application> getApplication() const noexcept override;

        [[nodiscard]] safe_ptr<IInputService> getInputService() const noexcept override;

        [[nodiscard]] safe_ptr<IWindowService> getWindowService() const noexcept override;

        [[nodiscard]] safe_ptr<IPlayerService> getPlayerService() const noexcept override;

        [[nodiscard]] std::error_code update(TimeSpan dt) override;

        // Collection-only native pump (B2' IPlatform::pumpNativeEvents
        // override): window-event callbacks stage owned records into the
        // input batch; no client callback runs until update() dispatches the
        // completed batches below. Repeatable within a frame: device time
        // never advances, transient character input is never cleared,
        // gamepads are never re-sampled or dispatched here.
        [[nodiscard]] std::error_code pumpNativeEvents() override;

    private:
        safe_ptr<Application> m_application{};

        std::unique_ptr<GlfwInput> m_input_service{};
        std::unique_ptr<GlfwPlayer> m_player_service{};
        std::unique_ptr<GlfwWindow> m_window_service{};
    };
}
