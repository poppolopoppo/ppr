module;

export module engine.app;

import engine.core;
import engine.platform;
import std;

export import :input_service;

export namespace pP {

    class Camera {
    public:
        math::float3 position{0.0f, 0.0f, 5.0f};
        math::float3 target{0.0f, 0.0f, 0.0f};
        math::float3 up{0.0f, 1.0f, 0.0f};
        float fovY = 1.047f;
        float aspect = 16.0f / 9.0f;
        float nearZ = 0.1f;
        float farZ = 100.0f;

        [[nodiscard]] math::float4x4 viewMatrix() const noexcept;
        [[nodiscard]] math::float4x4 projectionMatrix() const noexcept;
        [[nodiscard]] math::float4x4 viewProjectionMatrix() const noexcept;

        [[nodiscard]] std::pair<math::float3, math::float3> screenToWorld(math::float2 screenPos) const noexcept;
    };

    class Application : public IApp {
    public:
        enum EExitCode : int {
            exit_no_error = 0,
            exit_exception = -1,
            exit_failed_init = -2,
        };

        Application(std::string_view name, std::span<const char* const> argv);
        ~Application() noexcept override;

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        [[nodiscard]] std::string_view getName() const noexcept { return m_name; }
        [[nodiscard]] std::string_view getVariant() const noexcept { return m_variant; }

        [[nodiscard]] std::span<const std::string> getArguments() const noexcept { return m_arguments; }

        [[nodiscard]] const std::filesystem::directory_entry& getInstallDir() const noexcept { return m_installDir; }
        [[nodiscard]] const std::filesystem::directory_entry& getConfigDir() const noexcept { return m_configDir; }
        [[nodiscard]] const std::filesystem::directory_entry& getContentDir() const noexcept { return m_contentDir; }
        [[nodiscard]] const std::filesystem::directory_entry& getWorkingDir() const noexcept { return m_workingDir; }

        [[nodiscard]] const ServiceLocator &getServices() const noexcept { return m_services; }

        void setExitCode(int exitCode) noexcept;

        [[nodiscard]] int run();

        // IApp overrides
        [[nodiscard]] bool onInitialize() override;
        [[nodiscard]] bool onUpdate(double deltaTime) override;
        void onRender() override;
        void onShutdown() override;

        // Accessors
        [[nodiscard]] IPlatform& getPlatform() const noexcept;
        [[nodiscard]] IWindow& getWindow() const noexcept;
        [[nodiscard]] Camera& getCamera() noexcept;

    private:
        std::vector<std::string> m_arguments{};
        ServiceLocator m_services{};

        std::string m_name{};
        std::string m_variant{};

        std::filesystem::directory_entry m_installDir{};
        std::filesystem::directory_entry m_configDir{};
        std::filesystem::directory_entry m_contentDir{};
        std::filesystem::directory_entry m_workingDir{};

        std::atomic<int> m_exitCode = 0;

        std::unique_ptr<IPlatform> m_platform{};
        std::unique_ptr<IWindow> m_window{};
        Camera m_camera{};
        double m_lastFrameTime = 0.0;
    };

}
