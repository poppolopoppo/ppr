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
    struct DetachedUiDrawData;
    struct DrawContext;

    class IUIService : public virtual IService {
    public:
        [[nodiscard]] virtual std::error_code initialize(
            WindowInputContext &window_input_context, IRhiService &rhi_service, IShaderService &shader_service,
            int input_listener_priority) = 0;

        [[nodiscard]] virtual std::error_code shutdown() = 0;

        // Cached mouse-capture flag for press-time sampling.
        [[nodiscard]] virtual bool hasMouseCaptureUnlessPopupClose() const noexcept = 0;

        // Clears UI window focus; call post-update with open frame.
        [[nodiscard]] virtual std::error_code clearWindowFocus() = 0;

        [[nodiscard]] virtual std::error_code update(TimeSpan dt, const WindowViewport &viewport) = 0;

        [[nodiscard]] virtual std::error_code render(const DrawContext &draw_context) = 0;

        // P2 frontend/backend split. buildDetachedUiFrame runs on the CLIENT
        // lane: it completes ImGui::Render() and publishes detached owned
        // draw data (deep-copied verts/indices/commands, baked clip/scale,
        // staged font pixels on rebuild). renderDetachedUiFrame runs on ANY
        // lane (inline or worker): it encodes ONLY the detached data plus
        // lane-confined GPU state, never the live context/IO/GetDrawData().
        // Font/texture create-replace carries explicit ownership: the
        // client stages owned pixels + a version, the lane installs them
        // and reports the applied version for acknowledgement.
        [[nodiscard]] virtual std::error_code buildDetachedUiFrame(DetachedUiDrawData &out) = 0;

        [[nodiscard]] virtual std::error_code renderDetachedUiFrame(
            const DetachedUiDrawData &detached, const DrawContext &draw_context) = 0;

        // Last font atlas version installed on the encode lane (the ack
        // the frame result carries back to the client).
        [[nodiscard]] virtual u64 installedFontVersion() const noexcept = 0;

        [[nodiscard]] virtual void *getContext() const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<const InputListener> getInputListener() const noexcept = 0;
    };
}
