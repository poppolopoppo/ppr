module;

export module engine.app:renderer.types;

import :window.viewport;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // renderer boundary types (RHI-facing, camera-free)
    // ------------------------------------------------------------------

    struct ColorTargetInfo final {
        rhi::Format m_format{rhi::Format::Undefined};
        // NOTE: explicit zeros — mango's default Vector ctor leaves
        // components indeterminate, and tests assert sane defaults.
        int2 m_extent{0, 0};
        u32 m_sample_count{1u};
    };

    struct RenderView final {
        rhi::Viewport m_viewport{};
        rhi::ScissorRect m_scissor{};
    };

    struct DrawContext final {
        const RenderView &m_view;
        const ColorTargetInfo &m_target;
    };

    using DrawCallback = std23::function_ref<std::error_code(rhi::IRenderPassEncoder &, const DrawContext &)>;

    struct DrawSubmission final {
        RenderView m_view{};
        // No default: function_ref is a non-owning view with no null state,
        // so submissions always name their encoder explicitly.
        DrawCallback m_encode_draws;
    };

    struct ColorPassOptions final {
        rhi::LoadOp m_load_op{rhi::LoadOp::Clear};
        rhi::StoreOp m_store_op{rhi::StoreOp::Store};
        float4 m_clear_color{0.1f, 0.1f, 0.2f, 1.0f};
    };

    [[nodiscard]] std::optional<RenderView> makeRenderView(const Viewport &viewport, const int2 &target_extent) noexcept;

    // ------------------------------------------------------------------
    // scene layer (camera-owned; beside the boundary, not in it)
    // ------------------------------------------------------------------

    struct CameraSnapshot;

    // Scene-owned pairing; lives here so submissions can name it without
    // the boundary importing the scene module (fwd-declared only).
    struct SceneView final {
        const CameraSnapshot &m_camera;
        RenderView m_render_view{};
    };
}
