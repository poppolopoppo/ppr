module;

#include "pP/Macros.h"

export module engine.app:ui.imgui;

import :renderer.frame_fence;

import std;
import engine.math;
import imgui;

export namespace pP {
    class IUIService;

    namespace ui {
        [[nodiscard]] std::unique_ptr<IUIService> createImGuiService();
    }

    // Fence-awareness seam for UI backends behind IUIService: the frame
    // owner reports the SUBMIT's fence value after a successful submit so
    // the backend can key its upload-slot reuse off submission completion.
    // Queried via dynamic_cast (same shape as the GlfwInput seam in the
    // Editor); a backend that does not implement it simply keeps its own
    // rule. Render-thread confined like every other UI call.
    class IFenceAwareUiService {
    public:
        virtual ~IFenceAwareUiService() = default;

        virtual void notifyFrameSubmitted(FenceCompletionValue submit_value) noexcept = 0;
    };

    [[nodiscard]] ImVec2 imVec(const float2 &v) noexcept {
        return ImVec2{v.x, v.y};
    }

    [[nodiscard]] ImVec4 imVec(const float4 &v) noexcept {
        return ImVec4{v.x, v.y, v.z, v.w};
    }

    [[nodiscard]] float2 imVec(const ImVec2 &v) noexcept {
        return float2{v.x, v.y};
    }

    [[nodiscard]] float4 imVec(const ImVec4 &v) noexcept {
        return float4{v.x, v.y, v.z, v.w};
    }
}
