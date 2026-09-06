module;

export module engine.app:application;

import :input.action;
import :input.listener;
import :renderer;
import :renderer.triangle_pass;
import :scene.camera;
import :scene.camera.controller;
import :service.window;
import :window.viewport;

import engine.core;
import std;

export namespace pP {
    class IInputService;
    class IUIService;
    class IPlatform;
    class IWindowService;

    class Application : public safe_object {
    public:
        Application(std::string_view name, std::span<const char * const> argv);

        // ReSharper disable once CppHidingFunction - safe_object dtor is non-virtual; hiding is benign
        virtual ~Application() noexcept;

        Application(const Application &) = delete;

        Application &operator =(const Application &) = delete;

        Application(Application &&) = delete;

        Application &operator =(Application &&) = delete;

        [[nodiscard]] std::string_view getName() const noexcept { return m_name; }
        [[nodiscard]] std::string_view getVariant() const noexcept { return m_variant; }

        [[nodiscard]] std::span<const std::string> getArguments() const noexcept { return m_arguments; }

        [[nodiscard]] const std::filesystem::directory_entry &getInstallDir() const noexcept { return m_installDir; }
        [[nodiscard]] const std::filesystem::directory_entry &getConfigDir() const noexcept { return m_configDir; }
        [[nodiscard]] const std::filesystem::directory_entry &getContentDir() const noexcept { return m_content_dir; }
        [[nodiscard]] const std::filesystem::directory_entry &getWorkingDir() const noexcept { return m_workingDir; }

        [[nodiscard]] const ServicesStore &getServices() const noexcept { return m_services; }

        [[nodiscard]] ServicesStore &getServices() noexcept { return m_services; }

        void requestApplicationExit() noexcept;

        [[nodiscard]] std::error_code run();

        using ApplicationCallback = BroadcastCallback<std::error_code (const Application &app)>;

    protected:
        [[nodiscard]] virtual std::error_code initialize();

        [[nodiscard]] virtual std::error_code update();

        [[nodiscard]] virtual std::error_code render();

        [[nodiscard]] virtual std::error_code shutdown() noexcept;

        [[nodiscard]] constexpr const SharedContext &getLifecycle() const noexcept { return m_lifecycle; }
        [[nodiscard]] constexpr const SharedWindow &getMainWindow() const noexcept { return m_main_window; }

        [[nodiscard]] ServicesStore &getUiServices() noexcept { return m_ui_services; }

    private:
        enum class EState : u8 {
            created,
            initialized,
        };

        // Hot (per-frame): cached service pointers and owned scene state.
        safe_ptr<IWindowService> m_cached_window_service{};
        safe_ptr<IInputService> m_cached_input_service{};
        std::unique_ptr<WindowInputContext> m_window_input{};
        InputListener m_scene_listener{};
        InputMapping m_scene_controller_mapping{"CameraController"};
        Camera m_scene_camera{};
        CameraModel m_scene_camera_model{};
        FreeCameraController m_scene_controller{};
        std::unique_ptr<WindowViewport> m_main_viewport{};
        Renderer m_renderer{};
        TrianglePass m_triangle_pass{};
        SharedWindow m_main_window{};
        SharedContext m_lifecycle{};
        std::chrono::steady_clock::time_point m_last_frame_time{std::chrono::steady_clock::now()};
        bool m_focused{true};

        // Cold (init/shutdown only)
        ServicesStore m_services{};
        ServicesStore m_ui_services{safe_ptr<ServicesStore>(&m_services)};
        context::CancelFunc m_cancel{};
        IWindowService::WindowResizedCallback::Handle m_resize_handle{};
        IWindowService::WindowFocusedCallback::Handle m_focus_handle{};
        IWindowService::WindowCallback::Handle m_close_handle{};

        std::error_code onWindowResized_(const Window &window, const int2 &old_size);

        std::error_code onWindowFocused_(const Window &window [[maybe_unused]], bool focused) noexcept;

        std::error_code onWindowClosed_(const Window &window [[maybe_unused]]) noexcept;

        EState m_state{EState::created};

        // Cold (init-time)
        std::unique_ptr<IUIService> m_ui_service;
        safe_ptr<IPlatform> m_platform;
        Array<std::string> m_arguments{};
        std::string m_name{};
        std::string m_variant{};

        std::filesystem::directory_entry m_installDir{};
        std::filesystem::directory_entry m_configDir{};
        std::filesystem::directory_entry m_content_dir{};
        std::filesystem::directory_entry m_workingDir{};
    };
}
