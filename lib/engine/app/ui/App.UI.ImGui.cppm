module;

#include "pP/Macros.h"

export module engine.app:ui.imgui;

import :renderer.frame_fence;

import engine.core;
import std;
import engine.math;
import imgui;

export namespace pP {
    class IUIService;

    namespace ui {
        [[nodiscard]] std::unique_ptr<IUIService> createImGuiService();
    }

    // P2 detached UI draw data: everything the render lane needs to encode
    // one frame's UI, owned and imgui-free. Published by the client AFTER
    // ImGui::Render() completes; immutable afterwards. The worker never
    // reads the live context, live IO, GetDrawData(), or mutable frontend
    // backend fields — only this struct.
    struct DetachedUiVertex {
        float2 m_position{};
        float2 m_uv{};
        u32 m_color{0u};
    };

    struct DetachedUiCommand {
        // Pre-scaled clip rectangle in framebuffer pixels (DisplayPos and
        // framebuffer scale are baked at detach time, never re-read live).
        float4 m_clip_rect{};
        // Opaque texture identity as observed at detach (the font atlas
        // view for the stock backend). Unknown identities are rejected at
        // encode, never dereferenced.
        u64 m_texture_id{0u};
        u32 m_element_count{0u};
        u32 m_index_offset{0u};
        u32 m_vertex_offset{0u};
        // Known render-only reset command (reapplies render state at this
        // position); any other client-context callback was already rejected
        // at detach and never reaches the lane.
        bool m_reset_render_state{false};
    };

    struct DetachedUiDrawData {
        u64 m_frame_id{0u};
        float2 m_display_size{};
        float2 m_framebuffer_scale{1.0f, 1.0f};
        float2 m_framebuffer_extent{};
        Array<DetachedUiVertex> m_vertices{};
        Array<u32> m_indices{};
        Array<DetachedUiCommand> m_commands{};
        // Font atlas publication: nonzero only when the client rebuilt the
        // atlas (DPI/scale change). The lane uploads the pixels, installs
        // them, and acknowledges m_font_version in the frame result. Empty
        // pixels mean "keep the installed atlas".
        u64 m_font_version{0u};
        int2 m_font_extent{zero_v};
        Array<u8> m_font_pixels{};
        bool m_has_content{false};
    };

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
