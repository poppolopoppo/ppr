module;

export module engine.app:application;

import engine.core;
import std;

export namespace pP {
    class IClientService;
    class IInputService;
    class IPlatform;
    class IUIService;
    class IWindowService;
    class Renderer;

    struct ApplicationDomain {
        /// headless applications run without a window
        bool m_is_headless: 1 {false};
        /// interactive applications may require user-input
        bool m_is_interactive: 1 {true};
        /// handle player profile and online services
        bool m_needs_presence: 1 {false};
        /// application needs to render something with the GPU
        bool m_needs_rendering: 1 {true};
        /// spawn ui service and render user-interfaces
        bool m_needs_user_interface: 1 {true};
    };

    class Application : public safe_object {
    public:
        Application(ApplicationDomain domain, std::string_view name, std::span<const char * const> argv);

        // ReSharper disable once CppHidingFunction - safe_object dtor is non-virtual; hiding is benign
        virtual ~Application() noexcept;

        Application(const Application &) = delete;

        Application &operator =(const Application &) = delete;

        Application(Application &&) = delete;

        Application &operator =(Application &&) = delete;

        [[nodiscard]] const std::string &getName() const noexcept { return m_name; }
        [[nodiscard]] std::span<const std::string> getArguments() const noexcept { return m_arguments; }

        [[nodiscard]] const ApplicationDomain &getDomain() const noexcept { return m_domain; }
        [[nodiscard]] const IPlatform &getPlatform() const noexcept { return *m_platform; }
        [[nodiscard]] const SharedContext &getLifecycle() const noexcept { return m_lifecycle; }
        [[nodiscard]] const Renderer &getRenderer() const noexcept { return *m_renderer; }
        [[nodiscard]] const ServicesStore &getServices() const noexcept { return m_services; }
        [[nodiscard]] const TimerManager &getTimerManager() const noexcept { return m_application_clock; }
        [[nodiscard]] const std::optional<TimeDuration> &getTargetFrameDuration() const noexcept;

        [[nodiscard]] Renderer &getRenderer() noexcept { return *m_renderer; }
        [[nodiscard]] ServicesStore &getServices() noexcept { return m_services; }
        [[nodiscard]] TimerManager &getTimerManager() noexcept { return m_application_clock; }

        [[nodiscard]] const std::filesystem::directory_entry &getInstallDir() const noexcept { return m_install_dir; }
        [[nodiscard]] const std::filesystem::directory_entry &getConfigDir() const noexcept { return m_config_dir; }
        [[nodiscard]] const std::filesystem::directory_entry &getContentDir() const noexcept { return m_content_dir; }
        [[nodiscard]] const std::filesystem::directory_entry &getWorkingDir() const noexcept { return m_working_dir; }

        void requestExit(std::error_code clause = {}) const noexcept;

        void setBackgroundPriority(bool throttle) noexcept;

        void setTargetFrameDuration(TimeDuration frame_time) noexcept;

        void setTargetFrameDuration(std::nullopt_t) noexcept;

        void setTargetFrameRate(int fps) noexcept;

        [[nodiscard]] std::error_code run();

    protected:
        [[nodiscard]] virtual std::error_code initialize();

        [[nodiscard]] virtual std::error_code update(TimeSpan dt);

        [[nodiscard]] virtual std::error_code render();

        [[nodiscard]] virtual std::error_code shutdown();

    private:
        std::unique_ptr<Renderer> m_renderer;
        TimerManager m_application_clock{};
        std::optional<TimeDuration> m_target_frame_duration{};

        // m_platform precedes m_services so reverse-destruction releases the
        // service-store observers before their platform owners (safe_ptr rule).
        const std::unique_ptr<IPlatform> m_platform{};
        ServicesStore m_services{};
        SharedContext m_lifecycle{};
        context::CancelClauseFunc m_request_exit{};

        /// throttle refresh loop to low frame-rate when application is in background
        bool m_has_background_priority{false};
        // Single-use latch: set on the first teardown attempt, never reset.
        // run()/initialize() after shutdown report operation_not_permitted.
        bool m_has_torn_down{false};

        const Array<std::string> m_arguments{};
        const std::string m_name{};
        const ApplicationDomain m_domain{};

        std::filesystem::directory_entry m_install_dir{};
        std::filesystem::directory_entry m_config_dir{};
        std::filesystem::directory_entry m_content_dir{};
        std::filesystem::directory_entry m_working_dir{};
    };
}
