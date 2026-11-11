module;
#include "pP/Macros.h"
export module engine.app:renderer.types;

import :renderer.frame_fence;

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
    // immutable world and committed particle input identity
    // ------------------------------------------------------------------

    // Value identity of an activated world bundle. Zero job/generation means
    // no activated world; the owner lane is the only reader of bundle state.
    struct GridWorldIdentity {
        u64 m_job_id{};
        u64 m_world_generation{};
        u64 m_content_fingerprint{};

        [[nodiscard]] bool operator==(const GridWorldIdentity &) const noexcept = default;
    };

    inline constexpr u32 kGridParticleNumericalLayout = 4u;
    inline constexpr u32 kGridParticleNumericalStride = 64u;
    inline constexpr u32 kGridParticleReadonlyStride = 24u;
    inline constexpr u32 kGridParticleCapacity = 1024u * 1024u;

    // Stable row/ID mapping and gas-only physical inputs. Moving radius is
    // cell_metres*sqrt(Cold24.volume/pi); gas radius is sqrt(volume/thickness/pi).
    // m_material_temperature packs material ID16 + IEEE binary16 Kelvin with
    // the same encoding as resolved cells; consumers mask the low material bits.
    struct GridParticleMetadataRow {
        u32 m_id_low{}, m_id_high{}, m_source_row{0xffff'ffffu}, m_material_temperature{};
        float m_gas_x{}, m_gas_y{}, m_radius{};
        // Zero explicitly means unavailable for legacy presentation-only input.
        // Otherwise binary32 V/(cell_metres^2*reference_thickness), before gas radius.
        u32 m_physical_fill_bits{};
    };

    static_assert(sizeof(GridParticleMetadataRow) == 32u and alignof(GridParticleMetadataRow) == 4u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_id_low) == 0u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_id_high) == 4u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_source_row) == 8u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_material_temperature) == 12u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_gas_x) == 16u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_gas_y) == 20u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_radius) == 24u);
    static_assert(PPR_OFFSETOF(GridParticleMetadataRow, m_physical_fill_bits) == 28u);
    static_assert(
        std::is_standard_layout_v<GridParticleMetadataRow> and std::is_trivially_copyable_v<GridParticleMetadataRow>);

    struct GridParticleMetadataResource {
        rhi::ComPtr<rhi::IBuffer> m_rows{};
        Array<u32> m_materials{};
        u64 m_version{};
        double m_cell_metres{};
        u32 m_count{}, m_moving_count{};
    };

    struct GridParticlePresentationIdentity {
        GridWorldIdentity m_world{};
        u64 m_publication{}, m_terrain_revision{}, m_joint_tick{};

        [[nodiscard]] bool operator==(const GridParticlePresentationIdentity &) const noexcept = default;
    };

    struct GridParticleGpuView {
        rhi::ComPtr<rhi::IBuffer> m_numerical{};
        rhi::ComPtr<rhi::IBuffer> m_readonly{};
        std::shared_ptr<const GridParticleMetadataResource> m_metadata{};
        std::shared_ptr<const std::atomic<u64>> m_live_epoch{};
        GridWorldIdentity m_world{};
        u64 m_device_epoch{}, m_publication{}, m_terrain_revision{}, m_joint_tick{};
        double m_origin{3.0};
        u32 m_layout{kGridParticleNumericalLayout};
        u32 m_stride{kGridParticleNumericalStride};
        u32 m_readonly_stride{kGridParticleReadonlyStride};

        [[nodiscard]] bool current() const noexcept {
            return m_numerical and
                   m_readonly and
                   m_layout == kGridParticleNumericalLayout and
                   m_stride == kGridParticleNumericalStride and
                   m_readonly_stride == kGridParticleReadonlyStride and
                   m_numerical->getDesc().elementSize == kGridParticleNumericalStride and
                   m_readonly->getDesc().elementSize == kGridParticleReadonlyStride and
                   m_live_epoch and
                   m_live_epoch->load(std::memory_order_acquire) == m_device_epoch;
        }
    };

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
            case 1u: return SampleCount::x1;
            case 2u: return SampleCount::x2;
            case 4u: return SampleCount::x4;
            case 8u: return SampleCount::x8;
            default: return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
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
            const RenderPipelineSignature &value, const hash_t seed = {hash::default_seed_v}) noexcept {
            return hash::combine(
                seed, hash::sizedRange(value.colorFormats()), value.m_depth_stencil_format, value.m_sample_count);
        }
    };

    using RenderPipelineKey = hash::Memoizer<RenderPipelineSignature>;

    // ------------------------------------------------------------------
    // draw submissions
    // ------------------------------------------------------------------

    // P1 fence-wait hook: value-scoped host wait for ring-slot and growth
    // gates. The ONE callable type for the member below and every
    // downstream parameter. Owning (move_only_function, same class as the
    // WakePredicate fix) so it default-constructs empty and bool-tests
    // before invocation; bound by the Renderer to the frame fence for the
    // duration of the synchronous encode, never stored, never transported.
    using FenceWaitHook = std23::move_only_function<std::error_code(FenceCompletionValue)>;

    // Borrowed only during synchronous frame encoding, before opening the render
    // pass. Content passes use this encoder for compute and resource transitions
    // in the same submission as their draws; the renderer owns no content state.
    struct PreRenderContext final {
        rhi::IDevice &m_device;
        rhi::ICommandEncoder &m_encoder;
        FenceCompletionValue m_completed_fence_value{kFenceValueNone};
        mutable FenceWaitHook m_wait_for_fence{};
    };

    using PreRenderCallback = std23::function_ref<std::error_code(const PreRenderContext &)>;

    // Command tail: encoded on the same encoder AFTER the render-pass encoder
    // has ended and before finish/submit. Numeric status copies belong here
    // because a pre-render callback cannot observe values the draws have not
    // written yet. Borrowed only; it cannot escape the synchronous call.
    using PostRenderCallback = std23::function_ref<std::error_code(const PreRenderContext &)>;

    struct DrawContext final {
        rhi::IDevice &m_device;
        rhi::IRenderPassEncoder &m_pass;
        const RenderPipelineKey &m_render_pipeline_key;
        rhi::Viewport m_viewport{};
        rhi::ScissorRect m_scissor{};
        int2 m_target_extent{};
        // P1 fence governance, set by the Renderer for every encode:
        // GPU completion known at frame start. Passes key ring-slot reuse
        // off this value (never CPU frame modulo or swapchain image count).
        // kFenceValueNone when nothing was ever submitted through the fence.
        FenceCompletionValue m_completed_fence_value{kFenceValueNone};
        // Mutable so invocation works through the const DrawContext
        // reference every render path consumes (MSVC's move_only_function
        // call operator is non-const); the hook itself is still bound once
        // per encode and never reseated afterwards.
        mutable FenceWaitHook m_wait_for_fence{};
    };

    using DrawCallback = std23::function_ref<std::error_code(const DrawContext &)>;

    namespace details {
        template<typename T>
        concept TDrawable = requires (T &drawable, const DrawContext &draw_context) {
            { drawable.render(draw_context) } -> std::same_as<std::error_code>;
        };
    }

    struct DrawSubmission final {
        string_literal m_description;
        DrawCallback m_encode_draws;
        std::optional<rhi::Viewport> m_viewport{};
        std::optional<rhi::ScissorRect> m_scissor{};

        DrawSubmission(
            const string_literal description, DrawCallback encode_draws) noexcept // NOLINT(*-pro-type-member-init)
            : m_description(description), m_encode_draws(std::move(encode_draws)) {
        }

        template<details::TDrawable T>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        DrawSubmission(T &drawable) noexcept // NOLINT(*-pro-type-member-init)
            : DrawSubmission(
                  string_literal{std::in_place, typeid(T).name()},
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
        std::optional<PreRenderCallback> m_prepare{};
    };
}
