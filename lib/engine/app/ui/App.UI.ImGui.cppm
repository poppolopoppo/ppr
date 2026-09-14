module;

#include "pP/Macros.h"

export module engine.app:ui.imgui;

import std;
import engine.math;

export import imgui;

export namespace pP {
    class IUIService;

    namespace ui {
        [[nodiscard]] std::unique_ptr<IUIService> createImGuiService();
    }

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
