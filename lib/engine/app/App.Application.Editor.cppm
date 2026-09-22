module;

export module engine.app:application_editor;

import :application;
import :renderer.triangle_pass;
import :service.client;
import :service.input;

import engine.core;
import engine.image;
import engine.mesh;
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

    public:
        // §7 scene flow: editor-owned SceneAsset + decoded ImageAssets +
        // pass-uploaded GPU handles. loadScene imports, decodes (mapFile for
        // file refs, embedded bytes otherwise) and uploads with partial
        // rollback; update() submits one instance per (scene instance, prim)
        // with the node world matrix; unloadScene releases in reverse order.
        [[nodiscard]] std::error_code loadScene(const std::filesystem::path &dir, std::string_view file);

        [[nodiscard]] std::error_code unloadScene();

        [[nodiscard]] TrianglePass &trianglePass() noexcept { return *m_triangle_pass; }

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

        [[nodiscard]] std::error_code submitSceneInstances_();

        mesh::SceneAsset m_scene{};
        Array<image::ImageAsset> m_images{};
        TrianglePass::UploadedScene m_uploaded_scene{};
        bool m_has_scene = false;

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
