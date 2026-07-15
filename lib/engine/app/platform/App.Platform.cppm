module;

export module engine.app:platform;

import engine.core;

export namespace pP {
    // ------------------------------------------------------------------
    // platform abstraction interface
    // ------------------------------------------------------------------

    class Application;

    class IInputService;
    class IWindowService;
    class IPlayerService;

    struct PlatformVersion {
        int m_major{none_v};
        int m_minor{none_v};
        int m_revision{none_v};
    };

    class IPlatform : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~IPlatform() = default;

        [[nodiscard]] virtual std::string_view getPlatformName() const noexcept = 0;

        [[nodiscard]] virtual PlatformVersion getPlatformVersion() const noexcept = 0;

        virtual void initializePlatform(Application &app) = 0;

        virtual void shutdownPlatform(Application &app) = 0;

        [[nodiscard]] virtual safe_ptr<IInputService> getInputService() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<IWindowService> getWindowService() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<IPlayerService> getPlayerService() const noexcept = 0;
    };
}
