module;

export module engine.app:application_editor;

import :application;
import :service.client;
import :service.input;

import engine.core;
import std;

export namespace pP {
    class FreeCameraController;
    class InputBackgroundLatch;
    class InputMapping;
    class TrianglePass;
    class Window;
    class WindowInputContext;

    class ApplicationEditor : public Application, protected IClientService {
    public:
        ApplicationEditor(std::string_view name, std::span<const char *const> argv);

        ~ApplicationEditor() noexcept override;

        // IClientService
        [[nodiscard]] safe_ptr<const Camera> getMainCamera() const noexcept override;

        [[nodiscard]] safe_ptr<const Player> getMainPlayer() const noexcept override;

        [[nodiscard]] safe_ptr<const WindowViewport> getMainViewport() const noexcept override;

        [[nodiscard]] safe_ptr<Camera> getMainCamera() noexcept override;

        [[nodiscard]] safe_ptr<Player> getMainPlayer() noexcept override;

        [[nodiscard]] safe_ptr<WindowViewport> getMainViewport() noexcept override;

        [[nodiscard]] safe_ptr<const InputContext> getMainInputContext() const noexcept override;

        [[nodiscard]] safe_ptr<InputContext> getMainInputContext() noexcept override;

    protected:
        // ReSharper disable once CppOverrideWithDifferentVisibility
        [[nodiscard]] safe_ptr<const Application> getApplication() const noexcept override { return this; }

        // Application:
        [[nodiscard]] std::error_code initialize() override;

        [[nodiscard]] std::error_code update(TimeSpan dt) override;

        [[nodiscard]] std::error_code render() override;

        [[nodiscard]] std::error_code shutdown() override;

    private:
        void onMainWindowFocused_(const Window &window, bool focused);

        std::unique_ptr<Player> m_player{};
        std::unique_ptr<Camera> m_camera{};
        std::unique_ptr<FreeCameraController> m_camera_controller{};
        std::unique_ptr<WindowInputContext> m_main_input_context{};
        std::unique_ptr<InputMapping> m_camera_input_mapping{};
        std::unique_ptr<IUIService> m_ui_service{};
        std::unique_ptr<WindowViewport> m_main_viewport{};
        std::unique_ptr<TrianglePass> m_triangle_pass{};

        // Background-drag actuator @ detector priority; registrar == owner.
        // Routing-owned latch; actuator borrows it (detach-before-destroy).
        std::unique_ptr<InputBackgroundLatch> m_input_background_latch{};

        IInputService::DeviceCallback::Handle m_device_disconnected_handle{};
    };
}
