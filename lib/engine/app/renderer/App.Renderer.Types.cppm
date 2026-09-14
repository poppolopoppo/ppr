module;
export module engine.app:renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // renderer boundary types (RHI-facing, camera-free)
    // ------------------------------------------------------------------

    class Renderer;

    struct RenderPipelineSignature {
        std::span<const rhi::Format> m_color_formats{};
        std::optional<rhi::Format> m_depth_stencil_format{};
        u32 m_sample_count{1u};

        [[nodiscard]] bool operator==(const RenderPipelineSignature &other) const noexcept {
            return m_sample_count == other.m_sample_count and
                   m_depth_stencil_format == other.m_depth_stencil_format and
                   std::ranges::equal(m_color_formats, other.m_color_formats);
        }

        [[nodiscard]] friend hash_t hashValue(
            const RenderPipelineSignature &value,
            const hash_t seed = {hash::default_seed_v}) noexcept {
            return hash::combine(seed,
                value.m_color_formats,
                value.m_depth_stencil_format,
                value.m_sample_count);
        }
    };

    using RenderPipelineKey = hash::Memoizer<RenderPipelineSignature>;

    struct DrawContext final {
        rhi::IDevice &m_device;
        rhi::IRenderPassEncoder &m_pass;
        const RenderPipelineKey &m_render_pipeline_key;
        rhi::Viewport m_viewport{};
        rhi::ScissorRect m_scissor{};
        int2 m_target_extent{};
    };

    using DrawCallback = std23::function_ref<std::error_code(const DrawContext &)>;

    namespace details {
        template<typename T>
        concept TDrawable = requires(T &drawable, const DrawContext &draw_context)
        {
            { drawable.render(draw_context) } -> std::same_as<std::error_code>;
        };
    }

    struct DrawSubmission final {
        string_literal m_description;
        DrawCallback m_encode_draws;
        std::optional<rhi::Viewport> m_viewport{};
        std::optional<rhi::ScissorRect> m_scissor{};

        DrawSubmission(const string_literal description, DrawCallback encode_draws) noexcept // NOLINT(*-pro-type-member-init)
            : m_description(description),
              m_encode_draws(std::move(encode_draws)) {
        }

        template<details::TDrawable T>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        DrawSubmission(T &drawable) noexcept // NOLINT(*-pro-type-member-init)
            : DrawSubmission(string_literal{std::in_place, typeid(T).name()},
                DrawCallback{std23::nontype<&T::render>, &drawable}) {
        }
    };

    struct ColorAttachmentOps {
        float4 m_clear_color{0.1f, 0.1f, 0.2f, 1.0f};
        rhi::LoadOp m_load_op{rhi::LoadOp::Clear};
        rhi::StoreOp m_store_op{rhi::StoreOp::Store};
    };

    struct SurfaceRenderPass {
        ColorAttachmentOps m_surface_color{};
        std::span<const rhi::RenderPassColorAttachment> m_additional_colors{};
        std::optional<rhi::RenderPassDepthStencilAttachment> m_depth_stencil{};
    };
}
