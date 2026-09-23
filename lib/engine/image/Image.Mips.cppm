module;
#include "pP/Macros.h"
export module engine.image:mips;

import :types;
import engine.core;
import engine.math;

import std;

// Phase 8 M1 CPU mip chains (docs/plans/asset-pipeline.md §4 chain design):
// float-linear workspace via stb_image_resize2 (vendored stb target, PRIVATE
// to engine.image; Mango has no float resample), halving to 4 px
// (mipCount = FloorLog2(min) - 1), sRGB to linear before the filter with
// re-encode after, alpha-coverage preserve by binary search (mask materials,
// upscale-only), chamfer distance-field plus color-expand anti-bleed. The
// chain lands in m_subresources[m_mip_count] with chain bytes counted against
// the decode cap; KTX2-embedded chains pass through untouched (blocked inputs
// reject here, never decode to recompress).

export namespace pP::image {
    struct MipGenDesc {
        bool m_high_quality = false;
        bool m_preserve_coverage = false;
        float m_alpha_cutoff = 0.5f;
        ImageLimits m_limits = kDefaultImageLimits;
    };

    // Incremental resampling (false, the default) filters each level from its
    // predecessor; high-quality mode (true, opt-in) filters every level from
    // the top level directly.

    // Halving to 4 px: FloorLog2(min) - 1 clamped to at least one level, so an
    // 8 px minimum yields the pair {8, 4} and anything at or below 4 px stays
    // a single level.
    [[nodiscard]] constexpr u32 mipCountFor(const u32 width, const u32 height) noexcept {
        const u32 small = width < height ? width : height;
        if (small <= 1u) {
            return 1u;
        }
        const u32 floor_log2 = std::bit_width(small) - 1u;
        return floor_log2 >= 2u ? floor_log2 - 1u : 1u;
    }

    // Halved extent at a chain level, floored at one pixel.
    [[nodiscard]] constexpr u32 mipExtentAt(const u32 base, const u32 level) noexcept {
        return (base >> level) > 0u ? base >> level : 1u;
    }

    // Builds the full chain in place: level 0 bytes are kept verbatim, levels
    // 1..mipCount-1 are appended into one frozen storage with tight pitches.
    // Blocked assets (KTX2/DDS passthrough) reject with function_not_supported
    // so embedded chains are never decoded to recompress; anything else
    // malformed rejects with invalid_argument.
    [[nodiscard]] std::error_code generateMipChain(ImageAsset &asset, MipGenDesc desc);
}
