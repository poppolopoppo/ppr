module;

export module engine.app:platform;

import engine.core;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // platform abstraction interface
    // ------------------------------------------------------------------

    class Application;

    class IInputService;
    class IWindowService;
    class IPlayerService;

    using SharedPlatform = safe_ptr<class IPlatform>;

    namespace platform {
        enum class errc : int {
            ok = 0,
            fail,
            initialization_failed,
            invalid_argument,
        };

        struct Version {
            int m_major{none_v};
            int m_minor{none_v};
            int m_revision{none_v};
        };

        [[nodiscard]] const std::error_category &error_category() noexcept;
        [[nodiscard]] std::error_code make_error_code(int result) noexcept;
        [[nodiscard]] std::error_code make_error_code(errc err) noexcept;
        [[nodiscard]] std::error_code result(int result) noexcept;
    }

    class IPlatform : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~IPlatform() = default;

        [[nodiscard]] static SharedPlatform get() noexcept;

        [[nodiscard]] virtual std::error_code initialize(Application &app) = 0;

        [[nodiscard]] virtual std::error_code shutdown(Application &app) = 0;

        [[nodiscard]] virtual std::string_view getPlatformName() const noexcept = 0;

        [[nodiscard]] virtual platform::Version getPlatformVersion() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<IInputService> getInputService() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<IWindowService> getWindowService() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<IPlayerService> getPlayerService() const noexcept = 0;
    };
}

export template<>
struct std::is_error_code_enum<pP::platform::errc> : true_type {
};
