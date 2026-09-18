module;
#include "pP/Macros.h"
export module engine.app:service.ui;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    class InputListener;
    class WindowInputContext;
    class WindowViewport;
    struct DrawContext;

    class IUIService : public virtual IService {
    public:
        [[nodiscard]] virtual std::error_code initialize(
            WindowInputContext &window_input_context,
            IRhiService &rhi_service,
            IShaderService &shader_service,
            int input_listener_priority) = 0;

        [[nodiscard]] virtual std::error_code shutdown() = 0;

        // Cached mouse-capture flag for press-time sampling.
        [[nodiscard]] virtual bool hasMouseCaptureUnlessPopupClose() const noexcept = 0;

        // Clears UI window focus; call post-update with open frame.
        [[nodiscard]] virtual std::error_code clearWindowFocus() = 0;

        [[nodiscard]] virtual std::error_code update(TimeSpan dt, const WindowViewport &viewport) = 0;

        [[nodiscard]] virtual std::error_code render(const DrawContext &draw_context) = 0;

        [[nodiscard]] virtual void *getContext() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<const InputListener> getInputListener() const noexcept = 0;
    };
}
