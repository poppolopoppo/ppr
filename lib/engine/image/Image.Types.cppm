module;
#include "pP/Macros.h"
export module engine.image:types;

import engine.core;
import engine.math;

import std;

// P1 frozen CPU image asset vocabulary (ImageAsset, ImageDecodeDesc, BlockTag,
// NativeImageFormat, ImageUsage, ImageDimension, image::errc;
// docs/plans/asset-pipeline.md §2.2). The engine.core/engine.math imports carry
// the plan graph edge; no mango/rhi types cross this boundary.

namespace pP {
    // P0 Gate 1 (docs/plans/asset-pipeline.md §2.1): GPU-resident handles must be
    // standard-layout and exactly 4 bytes. Local tags only — P1 vocabulary replaces them.
    struct GpuU32TestTag final {
    };

    struct GpuKeyTestTag final {
    };

    static_assert(std::is_standard_layout_v<Numeric<u32, GpuU32TestTag> >);
    static_assert(sizeof(Numeric<u32, GpuU32TestTag>) == 4u);
    static_assert(std::is_standard_layout_v<Numeric<SparseKeyId, GpuKeyTestTag> >);
    static_assert(sizeof(Numeric<SparseKeyId, GpuKeyTestTag>) == 4u);
}

export namespace pP::image {
    // P1 frozen contract (docs/plans/asset-pipeline.md §2.2): CPU-only vocabulary.
    // No mango/rhi types cross — native format + plain layout; RHI mapping happens
    // at upload inside engine.app caches.
    enum class errc : int {
        ok = 0,
        invalid_argument = 1,
        function_not_supported = 2,
    };

    [[nodiscard]] const std::error_category &error_category() noexcept;
    [[nodiscard]] std::error_code make_error_code(errc err) noexcept;

    enum class BlockTag : u32 { none, bc1, bc3, bc4, bc5, bc7, astc4x4, astc6x6, astc8x8 };

    enum class NativeImageFormat : u32 {
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

    enum class ImageUsage : u8 { color, data };
    enum class ImageDimension : u8 { image2d };

    struct ImageDecodeDesc {
        bool m_simd = true;
        bool m_multithread = false;
        bool m_flip_v = false;
    };
    // m_multithread=false is engine POLICY (no Mango pool in the RT path), not the Mango default.
    // m_flip_v=false: glTF/Mango V already matches (import_gltf.cpp:740-742).

    struct ImageSubresource {
        mem::SharedBuffer m_view;
        u64 m_row_pitch = 0u;
        u64 m_slice_pitch = 0u;
    };

    struct ImageAsset {
        u32 m_width = 0u;
        u32 m_height = 0u;
        u32 m_mip_count = 1u;
        ImageDimension m_dimension = ImageDimension::image2d;
        NativeImageFormat m_format = NativeImageFormat::rgba8_linear;
        BlockTag m_tag = BlockTag::none;
        u32 m_block_w = 1u;
        u32 m_block_h = 1u;
        u32 m_bytes_per_block = 4u;
        bool m_is_srgb = false;
        bool m_is_block = false;
        mem::SharedBuffer m_storage;
        Array<ImageSubresource> m_subresources;
    };
    // Invariants (checked at decode return; PPR_ASSERT + invalid_argument on violation):
    // - m_dimension is ALWAYS image2d in MVP; 1D/3D/arrays/cubemaps are REJECTED with
    //   function_not_supported (or fully represented if scope widens — never silently treated as 2D).
    // - m_is_block == (m_tag != BlockTag::none); m_is_srgb == isSrgb(m_format).
    // - Unblocked: m_block_w == m_block_h == 1, m_bytes_per_block == 4,
    //   m_subresources.size() == m_mip_count.
    // - Blocked: block extents/bytes match m_tag;
    //   row_pitch == ceil(w/bw)*bytesPerBlock (derive, never duplicate).

    [[nodiscard]] constexpr bool isSrgb(const NativeImageFormat format) noexcept {
        switch (format) {
            case NativeImageFormat::rgba8_srgb:
            case NativeImageFormat::bc1_srgb:
            case NativeImageFormat::bc3_srgb:
            case NativeImageFormat::bc7_srgb:
            case NativeImageFormat::astc4x4_srgb:
            case NativeImageFormat::astc6x6_srgb:
            case NativeImageFormat::astc8x8_srgb: return true;
            default: return false;
        }
    }

    [[nodiscard]] constexpr bool isBlocked(const NativeImageFormat format) noexcept {
        return format != NativeImageFormat::rgba8_linear and format != NativeImageFormat::rgba8_srgb;
    }

    [[nodiscard]] constexpr BlockTag blockTagOf(const NativeImageFormat format) noexcept {
        switch (format) {
            case NativeImageFormat::bc1_linear:
            case NativeImageFormat::bc1_srgb: return BlockTag::bc1;
            case NativeImageFormat::bc3_linear:
            case NativeImageFormat::bc3_srgb: return BlockTag::bc3;
            case NativeImageFormat::bc4_linear: return BlockTag::bc4;
            case NativeImageFormat::bc5_linear: return BlockTag::bc5;
            case NativeImageFormat::bc7_linear:
            case NativeImageFormat::bc7_srgb: return BlockTag::bc7;
            case NativeImageFormat::astc4x4_linear:
            case NativeImageFormat::astc4x4_srgb: return BlockTag::astc4x4;
            case NativeImageFormat::astc6x6_linear:
            case NativeImageFormat::astc6x6_srgb: return BlockTag::astc6x6;
            case NativeImageFormat::astc8x8_linear:
            case NativeImageFormat::astc8x8_srgb: return BlockTag::astc8x8;
            default: return BlockTag::none;
        }
    }

    [[nodiscard]] constexpr u32 blockWidthOf(const BlockTag tag) noexcept {
        switch (tag) {
            case BlockTag::astc6x6: return 6u;
            case BlockTag::astc8x8: return 8u;
            case BlockTag::none: return 1u;
            default: return 4u;
        }
    }

    [[nodiscard]] constexpr u32 blockHeightOf(const BlockTag tag) noexcept {
        switch (tag) {
            case BlockTag::astc6x6: return 6u;
            case BlockTag::astc8x8: return 8u;
            case BlockTag::none: return 1u;
            default: return 4u;
        }
    }

    [[nodiscard]] constexpr u32 bytesPerBlockOf(const BlockTag tag) noexcept {
        switch (tag) {
            case BlockTag::bc1:
            case BlockTag::bc4: return 8u;
            case BlockTag::none: return 4u;
            default: return 16u;
        }
    }

    [[nodiscard]] constexpr u64 rowPitchFor(const u32 width, const BlockTag tag) noexcept {
        if (tag == BlockTag::none) [[likely]] {
            return static_cast<u64>(width) * 4u;
        }
        const u64 blocks_x = (static_cast<u64>(width) + blockWidthOf(tag) - 1u) / blockWidthOf(tag);
        return blocks_x * bytesPerBlockOf(tag);
    }

    [[nodiscard]] constexpr u64 slicePitchFor(const u32 width, const u32 height, const BlockTag tag) noexcept {
        if (tag == BlockTag::none) [[likely]] {
            return rowPitchFor(width, tag) * height;
        }
        const u64 blocks_y = (static_cast<u64>(height) + blockHeightOf(tag) - 1u) / blockHeightOf(tag);
        return rowPitchFor(width, tag) * blocks_y;
    }

    [[nodiscard]] inline hash_t contentHash(const mem::SharedBufferView view) noexcept {
        return hash::contiguousRange(view);
    }
    // Cross-load dedup key; NEVER hashValue(SharedBuffer) (owner identity).
}

export template<>
struct std::is_error_code_enum<pP::image::errc> : std::true_type {
};
