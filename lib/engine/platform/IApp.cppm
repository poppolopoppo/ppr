module;
#include "pP/Macros.h"
export module engine.platform:app;
import std;

export namespace pP {
    class IApp {
    public:
        virtual ~IApp() noexcept = default;

        [[nodiscard]] virtual bool onInitialize() = 0;
        [[nodiscard]] virtual bool onUpdate(double deltaTime) = 0;
        virtual void onRender() = 0;
        virtual void onShutdown() = 0;
    };
}
