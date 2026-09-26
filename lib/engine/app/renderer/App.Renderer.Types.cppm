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

    // ------------------------------------------------------------------
    // sample counts
    // ------------------------------------------------------------------

    // The accepted multisample counts as a CLOSED set: the power-of-two values
    // {1, 2, 4, 8}. No other value is representable, so a signature can only
    // carry an accepted count and every pass accepts by construction — there
    // is no runtime allowlist left for passes to drift from.
    enum class SampleCount : u8 {
        x1 = 1u,
        x2 = 2u,
        x4 = 4u,
        x8 = 8u,
    };

    // The single u32 -> SampleCount mapping: the RHI already rejects a
    // non-power-of-two count when the texture is created, but a value that
    // ever reaches this seam fails closed instead of narrowing into a
    // signature.
    [[nodiscard]] constexpr Expected<SampleCount> makeSampleCount(const u32 sample_count) noexcept {
        switch (sample_count) {
            case 1u:
                return SampleCount::x1;
            case 2u:
                return SampleCount::x2;
            case 4u:
                return SampleCount::x4;
            case 8u:
                return SampleCount::x8;
            default:
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
    }

    // ------------------------------------------------------------------
    // pipeline signatures
    // ------------------------------------------------------------------

    // Pipeline identity ONLY: the color formats, the sample count, and the
    // depth-stencil format. Temporal jitter (TAA/JBS offsets, a jittered
    // projection) must NEVER enter this type — a jittered signature compares
    // unequal every frame and would drop every cached pipeline on every frame.
    struct RenderPipelineSignature {
        static constexpr u32 kMaxColorFormats{4u};

        std::array<rhi::Format, kMaxColorFormats> m_color_formats{};
        u8 m_color_format_count{0u};
        std::optional<rhi::Format> m_depth_stencil_format{};
        SampleCount m_sample_count{SampleCount::x1};

        // Fail-safe view: the count is a public field, so clamp it — a stale
        // or out-of-range count can never build an out-of-bounds span.
        [[nodiscard]] std::span<const rhi::Format> colorFormats() const noexcept {
            const u32 count = std::min<u32>(m_color_format_count, kMaxColorFormats);
            return {m_color_formats.data(), count};
        }

        [[nodiscard]] bool operator==(const RenderPipelineSignature &other) const noexcept {
            return m_sample_count == other.m_sample_count and
                   m_depth_stencil_format == other.m_depth_stencil_format and
                   m_color_format_count == other.m_color_format_count and
                   std::ranges::equal(colorFormats(), other.colorFormats());
        }

        [[nodiscard]] friend hash_t hashValue(
            const RenderPipelineSignature &value,
            const hash_t seed = {hash::default_seed_v}) noexcept {
            return hash::combine(seed,
                hash::sizedRange(value.colorFormats()),
                value.m_depth_stencil_format,
                value.m_sample_count);
        }
    };

    using RenderPipelineKey = hash::Memoizer<RenderPipelineSignature>;

    // ------------------------------------------------------------------
    // draw submissions
    // ------------------------------------------------------------------

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

    // ------------------------------------------------------------------
    // attachment descriptions
    // ------------------------------------------------------------------

    struct ColorAttachmentOps {
        float4 m_clear_color{0.1f, 0.1f, 0.2f, 1.0f};
        rhi::LoadOp m_load_op{rhi::LoadOp::Clear};
        rhi::StoreOp m_store_op{rhi::StoreOp::Store};
    };

    enum class ESurfaceDepthPolicy : u8 {
        none,
        renderer_owned,
        external,
    };

    struct SurfaceRenderPass {
        ColorAttachmentOps m_surface_color{};
        std::initializer_list<rhi::RenderPassColorAttachment> m_additional_colors;
        ESurfaceDepthPolicy m_depth_policy{ESurfaceDepthPolicy::none};
        std::optional<rhi::RenderPassDepthStencilAttachment> m_depth_stencil{};
        std::initializer_list<const DrawSubmission> m_draws;
    };
}
