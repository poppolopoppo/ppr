module;
#include "pP/Macros.h"
export module engine.platform:window;
import std;

export namespace pP {
    enum class CursorMode { Normal, Hidden, Disabled };
    enum class KeyAction { Release, Press, Repeat };

    class IWindow {
    public:
        virtual ~IWindow() noexcept = default;

        [[nodiscard]] virtual bool shouldClose() const noexcept = 0;
        virtual void swapBuffers() = 0;
        virtual void setTitle(std::string_view title) = 0;
        virtual void setCursorMode(CursorMode mode) = 0;
        [[nodiscard]] virtual CursorMode getCursorMode() const noexcept = 0;
        [[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
    };
}
