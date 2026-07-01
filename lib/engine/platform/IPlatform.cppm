module;
#include "pP/Macros.h"
export module engine.platform:platform;
import std;

export namespace pP {
    class IWindow;
    struct WindowDesc {
        std::string title;
        int width = 1280;
        int height = 720;
        bool fullscreen = false;
        bool resizable = true;
    };

    class IPlatform {
    public:
        virtual ~IPlatform() noexcept = default;

        [[nodiscard]] virtual std::expected<std::unique_ptr<IWindow>, int> createWindow(const WindowDesc& desc) = 0;
        virtual void processEvents() = 0;
        [[nodiscard]] virtual std::span<const char* const> getRequiredInstanceExtensions() const = 0;
        [[nodiscard]] virtual double getTime() const noexcept = 0;
    };
}
