module;

export module engine.app:application_editor;

import :application;
import :service.client;

import engine.core;
import std;

export namespace pP {
    class TrianglePass;
    class Window;
    class WindowInputContext;
    class InputMapping;

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
        std::unique_ptr<Player> m_player{};
        std::unique_ptr<Camera> m_camera{};
        std::unique_ptr<ICameraController> m_camera_controller{};
        std::unique_ptr<WindowInputContext> m_main_input_context{};
        std::unique_ptr<InputMapping> m_camera_input_mapping{};
        std::unique_ptr<IUIService> m_ui_service{};
        std::unique_ptr<WindowViewport> m_main_viewport{};
        std::unique_ptr<TrianglePass> m_triangle_pass{};

        void onMainWindowClosed_(const Window &window) noexcept;
    };
}
