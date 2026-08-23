module;

export module engine.app:viewport;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    enum class EViewportFlags : u8 {
        None = 0,
        All = 0xFF,
    };

    // ------------------------------------------------------------------
    // ViewportConfig — viewport configuration
    // ------------------------------------------------------------------

    struct ViewportConfig {
        int2 framebuffer_size{0};
    };

    // ------------------------------------------------------------------
    // ViewportEntry — per-frame viewport render entry
    // ------------------------------------------------------------------

    struct ViewportEntry {
        rhi::ComPtr<rhi::IRenderPipeline> pipeline;
        rhi::Viewport viewport{};
        rhi::ScissorRect scissor{};
        // Draw callback receives the entry's viewport/scissor and must set the
        // complete RenderState itself, because RenderPassEncoder::setRenderState
        // replaces the whole state on every call (a buffers-only call would wipe
        // the viewport/scissor set by the caller).
        std23::function_ref<std::error_code(rhi::IRenderPassEncoder &, const rhi::Viewport &, const rhi::ScissorRect &)> draw;
    };

    // ------------------------------------------------------------------
    // Viewport — viewport with client-rect coordinate transforms
    // ------------------------------------------------------------------

    class Viewport {
    public:
        Viewport() noexcept = default;
        Viewport(const int2 &clientRect, EViewportFlags flags = EViewportFlags::None) noexcept;

        [[nodiscard]] const int2 &clientRect() const noexcept { return m_clientRect; }
        void setClientRect(const int2 &value) noexcept;

        [[nodiscard]] EViewportFlags flags() const noexcept { return m_flags; }
        void setFlags(EViewportFlags value) noexcept { m_flags = value; }

        [[nodiscard]] int2 screenToClient(const int2 &screenPos) const noexcept;
        [[nodiscard]] int2 clientToScreen(const int2 &clientPos) const noexcept;
        [[nodiscard]] float2 clientToTexCoord(const int2 &clientPos) const noexcept;
        [[nodiscard]] int2 texCoordToClient(const float2 &texCoord) const noexcept;

    private:
        int2 m_clientRect{zero_v};
        EViewportFlags m_flags{EViewportFlags::None};
    };
}