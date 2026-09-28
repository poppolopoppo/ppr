module;
#include "pP/Macros.h"
export module engine.image:types;

import engine.core;
import engine.math;

import std;

export namespace pP::image {
    // ------------------------------------------------------------------
    // image errors and formats
    // ------------------------------------------------------------------

    PPR_DECLARE_LOG_CATEGORY(Image);

    // CPU-only vocabulary. No mango/rhi types cross — native format + plain layout; RHI mapping happens
    // at upload inside engine.app caches.
    enum class errc : int {
        ok = 0,
        invalid_argument = 1,
        function_not_supported = 2,
    };

    [[nodiscard]] const std::error_category &error_category() noexcept;

    [[nodiscard]] std::error_code make_error_code(errc err) noexcept;

    enum class EBlockTag : u32 {
        none,

        bc1,
        bc3,
        bc4,
        bc5,
        bc7,

        astc4x4,
        astc6x6,
        astc8x8
    };

    enum class ENativeImageFormat : u32 {
        rgba8_linear,
        rgba8_srgb,

        bc1_linear,
        bc1_srgb,
        bc3_linear,
        bc3_srgb,
        bc4_linear,
        bc5_linear,
        bc7_linear,
        bc7_srgb,

        astc4x4_linear,
        astc4x4_srgb,
        astc6x6_linear,
        astc6x6_srgb,
        astc8x8_linear,
        astc8x8_srgb
    };

    enum class EImageUsage : u8 { color, data };

    enum class EImageDimension : u8 { image2d };

    // Configurable production limits: every decode rejects over-limit inputs fail-closed
    // with invalid_argument (no new errc — the taxonomy already covers deterministic
    // rejection). Width and
    // height bound header claims before any allocation; decoded bytes bound
    // the allocation itself (u64 compare, never narrowed first); input bytes
    // bound header-parse work. Defaults are production: 8K RGBA is exactly
    // 256 MiB, so dimensions and bytes agree.
    struct ImageLimits {
        u32 m_max_width = 8192u;
        u32 m_max_height = 8192u;
        u64 m_max_decoded_bytes = 268435456u;
        u64 m_max_input_bytes = 268435456u;
    };

    inline constexpr ImageLimits kDefaultImageLimits{};

    struct ImageDecodeDesc {
        ImageLimits m_limits = kDefaultImageLimits;

        bool m_use_simd = true;
        bool m_use_multithread = false;
        bool m_use_flip_v = false;
    };

    // m_multithread=false is engine POLICY (no Mango pool in the RT path), not the Mango default.
    // m_flip_v=false: glTF/Mango V already matches (import_gltf.cpp:740-742).

    // ------------------------------------------------------------------
    // image asset descriptions and storage
    // ------------------------------------------------------------------

    struct ImageSubresource {
        mem::SharedBuffer m_view{};

        u64 m_row_pitch = 0u;
        u64 m_slice_pitch = 0u;
    };

    struct ImageAsset {
        mem::SharedBuffer m_storage{};
        Array<ImageSubresource> m_subresources{};

        u32 m_width = 0u;
        u32 m_height = 0u;
        u32 m_mip_count = 1u;

        u32 m_block_w = 1u;
        u32 m_block_h = 1u;
        u32 m_bytes_per_block = 4u;

        EImageDimension m_dimension = EImageDimension::image2d;
        ENativeImageFormat m_format = ENativeImageFormat::rgba8_linear;
        EBlockTag m_tag = EBlockTag::none;

        bool m_is_srgb = false;
        bool m_is_block = false;
    };

    // Invariants (checked at decode return; PPR_ASSERT + invalid_argument on violation):
    // - m_dimension is ALWAYS image2d; 1D/3D/arrays/cubemaps are REJECTED with
    //   function_not_supported (or fully represented if scope widens — never silently treated as 2D).
    // - m_is_block == (m_tag != BlockTag::none); m_is_srgb == isSrgb(m_format).
    // - Unblocked: m_block_w == m_block_h == 1, m_bytes_per_block == 4,
    //   m_subresources.size() == m_mip_count.
    // - Blocked: block extents/bytes match m_tag;
    //   row_pitch == ceil(w/bw)*bytesPerBlock (derive, never duplicate).

    // ------------------------------------------------------------------
    // format and block geometry
    // ------------------------------------------------------------------

    [[nodiscard]] constexpr bool isSrgb(const ENativeImageFormat format) noexcept {
        switch (format) {
            case ENativeImageFormat::rgba8_srgb:
            case ENativeImageFormat::bc1_srgb:
            case ENativeImageFormat::bc3_srgb:
            case ENativeImageFormat::bc7_srgb:
            case ENativeImageFormat::astc4x4_srgb:
            case ENativeImageFormat::astc6x6_srgb:
            case ENativeImageFormat::astc8x8_srgb: return true;
            default: return false;
        }
    }

    [[nodiscard]] constexpr bool isBlocked(const ENativeImageFormat format) noexcept {
        return format != ENativeImageFormat::rgba8_linear and format != ENativeImageFormat::rgba8_srgb;
    }

    [[nodiscard]] constexpr EBlockTag blockTagOf(const ENativeImageFormat format) noexcept {
        switch (format) {
            case ENativeImageFormat::bc1_linear:
            case ENativeImageFormat::bc1_srgb: return EBlockTag::bc1;
            case ENativeImageFormat::bc3_linear:
            case ENativeImageFormat::bc3_srgb: return EBlockTag::bc3;
            case ENativeImageFormat::bc4_linear: return EBlockTag::bc4;
            case ENativeImageFormat::bc5_linear: return EBlockTag::bc5;
            case ENativeImageFormat::bc7_linear:
            case ENativeImageFormat::bc7_srgb: return EBlockTag::bc7;
            case ENativeImageFormat::astc4x4_linear:
            case ENativeImageFormat::astc4x4_srgb: return EBlockTag::astc4x4;
            case ENativeImageFormat::astc6x6_linear:
            case ENativeImageFormat::astc6x6_srgb: return EBlockTag::astc6x6;
            case ENativeImageFormat::astc8x8_linear:
            case ENativeImageFormat::astc8x8_srgb: return EBlockTag::astc8x8;
            default: return EBlockTag::none;
        }
    }

    [[nodiscard]] constexpr u32 blockWidthOf(const EBlockTag tag) noexcept {
        switch (tag) {
            case EBlockTag::astc6x6: return 6u;
            case EBlockTag::astc8x8: return 8u;
            case EBlockTag::none: return 1u;
            default: return 4u;
        }
    }

    [[nodiscard]] constexpr u32 blockHeightOf(const EBlockTag tag) noexcept {
        switch (tag) {
            case EBlockTag::astc6x6: return 6u;
            case EBlockTag::astc8x8: return 8u;
            case EBlockTag::none: return 1u;
            default: return 4u;
        }
    }

    [[nodiscard]] constexpr u32 bytesPerBlockOf(const EBlockTag tag) noexcept {
        switch (tag) {
            case EBlockTag::bc1:
            case EBlockTag::bc4: return 8u;
            case EBlockTag::none: return 4u;
            default: return 16u;
        }
    }

    [[nodiscard]] constexpr u64 rowPitchFor(const u32 width, const EBlockTag tag) noexcept {
        if (tag == EBlockTag::none) [[likely]] {
            return static_cast<u64>(width) * 4u;
        }
        const u64 blocks_x = (static_cast<u64>(width) + blockWidthOf(tag) - 1u) / blockWidthOf(tag);
        return blocks_x * bytesPerBlockOf(tag);
    }

    [[nodiscard]] constexpr u64 slicePitchFor(const u32 width, const u32 height, const EBlockTag tag) noexcept {
        if (tag == EBlockTag::none) [[likely]] {
            return rowPitchFor(width, tag) * height;
        }
        const u64 blocks_y = (static_cast<u64>(height) + blockHeightOf(tag) - 1u) / blockHeightOf(tag);
        return rowPitchFor(width, tag) * blocks_y;
    }

    // ------------------------------------------------------------------
    // content hashing
    // ------------------------------------------------------------------

    // Cross-load dedup key; NEVER hashValue(SharedBuffer) (owner identity).
    [[nodiscard]] inline hash_t contentHash(const mem::SharedBufferView view) noexcept {
        return hash::contiguousRange(view);
    }
}

export template<>
struct std::is_error_code_enum<pP::image::errc> : std::true_type {
};
