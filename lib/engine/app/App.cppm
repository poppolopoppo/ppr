module;

export module engine.app;

import std;

export namespace app {

    class Application {
    public:
        Application(const std::string_view name, const std::span<const char* const> argv);
        virtual ~Application() noexcept;

        Application(const Application&) = delete;
        Application& operator =(const Application&) = delete;

        Application(Application&&) = delete;
        Application& operator =(Application&&) = delete;

        [[nodiscard]] std::string_view getName() const noexcept { return m_name; }
        [[nodiscard]] std::string_view getVariant() const noexcept { return m_variant; }

        [[nodiscard]] std::span<const std::string> getArguments() const noexcept { return m_arguments; }

        [[nodiscard]] const std::filesystem::directory_entry& getInstallDir() const noexcept { return m_installDir; }
        [[nodiscard]] const std::filesystem::directory_entry& getConfigDir() const noexcept { return m_configDir; }
        [[nodiscard]] const std::filesystem::directory_entry& getContentDir() const noexcept { return m_contentDir; }
        [[nodiscard]] const std::filesystem::directory_entry& getWorkingDir() const noexcept { return m_workingDir; }

        void setExitCode(const int exitCode) noexcept { m_exitCode = exitCode; }

        [[nodiscard]] int run();

    protected:
        [[nodiscard]] virtual bool initialize();
        [[nodiscard]] virtual bool update();
        virtual void render();
        virtual void terminate();

    private:
        std::vector<std::string> m_arguments;

        std::string m_name;
        std::string m_variant;

        std::filesystem::directory_entry m_installDir;
        std::filesystem::directory_entry m_configDir;
        std::filesystem::directory_entry m_contentDir;
        std::filesystem::directory_entry m_workingDir;

        std::atomic<int> m_exitCode = 0;
    };

}
