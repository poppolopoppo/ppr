module;

export module engine.app:application;

import :window.handle;
import :viewport;
import engine.core;
import std;

export namespace pP {
    class IInputService;
    class IUIService;
    class IPlatform;
    class IWindowService;
    class Renderer;

    class Application : public safe_object {
    public:
        Application(std::string_view name, std::span<const char * const> argv);

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

        [[nodiscard]] bool shouldClose() const noexcept { return m_should_close; }

        void requestApplicationExit() noexcept;

        [[nodiscard]] std::error_code run();

        using ApplicationCallback = Callback<std::error_code (const Application &app)>;

    protected:
        [[nodiscard]] virtual std::error_code initialize();

        [[nodiscard]] virtual std::error_code update();

        [[nodiscard]] virtual std::error_code render();

        [[nodiscard]] virtual std::error_code shutdown() noexcept;

        [[nodiscard]] constexpr const SharedContext &getLifecycle() const noexcept { return m_lifecycle; }
        [[nodiscard]] constexpr const SharedWindow &getMainWindow() const noexcept { return m_main_window; }

    private:
        enum class EState : u8 {
            created,
            initialized,
            terminated,
        };

        // Hot (per-frame) — first cache line
        safe_ptr<IWindowService> m_cached_window_service{};
        safe_ptr<IInputService> m_cached_input_service{};
        SharedWindow m_main_window{};
        SharedContext m_lifecycle{};
        std::chrono::steady_clock::time_point m_last_frame_time{std::chrono::steady_clock::now()};
        bool m_should_close{false};

        // Cold (init/shutdown only)
        ServicesStore m_services{};
        ServicesStore m_scene_services{safe_ptr<ServicesStore>(&m_services)};
        ServicesStore m_ui_services{safe_ptr<ServicesStore>(&m_services)};
        ViewportConfig m_scene_viewport{};
        ViewportConfig m_ui_viewport{};
        context::CancelFunc m_cancel{};
        WindowCallback<int2>::Handle m_resize_handle{};
        EState m_state{EState::created};

        // Cold (init-time)
        std::unique_ptr<IUIService> m_ui_service;
        std::unique_ptr<Renderer> m_renderer;
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
