module;

#include "pP/Macros.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#include <mango/math/srgb.hpp>

module engine.image;

import :types;
import :mips;
import engine.core;
import engine.math;

import std;

namespace pP::image {
    namespace {
        // ------------------------------------------------------------------
        // color transfer
        // ------------------------------------------------------------------

        // Mango's float sRGB transfer functions keep the resample in its
        // linear workspace. Alpha rides unmodified (already linear).

        [[nodiscard]] float byteToUnit_(const std::byte b) noexcept {
            return static_cast<float>(std::to_integer<unsigned int>(b)) / 255.0f;
        }

        [[nodiscard]] std::byte unitToByte_(const float v) noexcept {
            const float clamped = std::clamp(v, 0.0f, 1.0f);
            return static_cast<std::byte>(static_cast<unsigned int>(std::lround(clamped * 255.0f)));
        }

        // ------------------------------------------------------------------
        // alpha-aware resampling
        // ------------------------------------------------------------------

        // Chamfer distance-field color expansion: transparent texels borrow the
        // nearest opaque color (two-pass 3x3 chamfer, orthogonal 1 and diagonal
        // sqrt(2)) so the downsample filter never drags clear-black into
        // translucent edges. Alpha is untouched; fully opaque images no-op.
        void expandTransparentColors_(std::vector<float> &rgba, const u32 width, const u32 height) {
            const std::size_t count = static_cast<std::size_t>(width) * height;
            constexpr float kInfinite = 1.0e30f;
            constexpr float kOrthogonal = 1.0f;
            constexpr float kDiagonal = 1.41421356f;
            std::vector<float> distance(count);
            std::vector<unsigned char> opaque(count);
            for (std::size_t i = 0u; i < count; ++i) {
                const bool is_opaque = rgba[i * 4u + 3u] > 0.0f;
                opaque[i] = is_opaque ? 1u : 0u;
                distance[i] = is_opaque ? 0.0f : kInfinite;
            }

            auto relax = [&](const std::size_t at, const std::size_t from, const float step) {
                const float candidate = distance[from] + step;
                if (candidate < distance[at]) {
                    distance[at] = candidate;
                    rgba[at * 4u + 0u] = rgba[from * 4u + 0u];
                    rgba[at * 4u + 1u] = rgba[from * 4u + 1u];
                    rgba[at * 4u + 2u] = rgba[from * 4u + 2u];
                }
            };
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    const std::size_t at = static_cast<std::size_t>(y) * width + x;
                    if (x > 0u) {
                        relax(at, at - 1u, kOrthogonal);
                    }
                    if (y > 0u) {
                        if (x > 0u) {
                            relax(at, at - width - 1u, kDiagonal);
                        }
                        relax(at, at - width, kOrthogonal);
                        if (x + 1u < width) {
                            relax(at, at - width + 1u, kDiagonal);
                        }
                    }
                }
            }

            for (u32 y = height; y > 0u; --y) {
                for (u32 x = width; x > 0u; --x) {
                    const std::size_t at = static_cast<std::size_t>(y - 1u) * width + (x - 1u);
                    if (x < width) {
                        relax(at, at + 1u, kOrthogonal);
                    }
                    if (y < height) {
                        if (x < width) {
                            relax(at, at + width + 1u, kDiagonal);
                        }
                        relax(at, at + width, kOrthogonal);
                        if (x > 1u) {
                            relax(at, at + width - 1u, kDiagonal);
                        }
                    }
                }
            }

            for (std::size_t i = 0u; i < count; ++i) {
                if (opaque[i] == 0u and distance[i] >= kInfinite) {
                    rgba[i * 4u + 0u] = 0.0f;
                    rgba[i * 4u + 1u] = 0.0f;
                    rgba[i * 4u + 2u] = 0.0f;
                }
            }
        }

        [[nodiscard]] float coverageOf_(const std::vector<float> &rgba, const float cutoff) noexcept {
            std::size_t covered = 0u;
            const std::size_t count = rgba.size() / 4u;
            for (std::size_t i = 0u; i < count; ++i) {
                if (rgba[i * 4u + 3u] >= cutoff) {
                    ++covered;
                }
            }
            return static_cast<float>(covered) / static_cast<float>(count);
        }

        [[nodiscard]] float coverageAtScale_(const std::vector<float> &rgba, const float scale, const float cutoff) noexcept {
            std::size_t covered = 0u;
            const std::size_t count = rgba.size() / 4u;
            for (std::size_t i = 0u; i < count; ++i) {
                if (std::min(1.0f, rgba[i * 4u + 3u] * scale) >= cutoff) {
                    ++covered;
                }
            }
            return static_cast<float>(covered) / static_cast<float>(count);
        }

        // Coverage preserve for mask/alpha-tested materials: binary-search the
        // smallest alpha upscale (20 steps) that restores the top-level
        // coverage. Upscale-only with abort-below-1 semantics: when the
        // unscaled level already meets coverage the alphas stay untouched
        // (never downscale). Best effort when the ceiling (every nonzero alpha
        // maxed) still falls short.
        void preserveCoverage_(std::vector<float> &rgba, const float target, const float cutoff) {
            if (coverageOf_(rgba, cutoff) >= target) {
                return;
            }
            float low = 1.0f;
            float high = 2.0f;
            while (coverageAtScale_(rgba, high, cutoff) < target and high < 1048576.0f) {
                high *= 2.0f;
            }
            for (int step = 0; step < 20; ++step) {
                const float mid = (low + high) * 0.5f;
                if (coverageAtScale_(rgba, mid, cutoff) < target) {
                    low = mid;
                } else {
                    high = mid;
                }
            }
            for (std::size_t i = 0u; i < rgba.size() / 4u; ++i) {
                rgba[i * 4u + 3u] = std::min(1.0f, rgba[i * 4u + 3u] * high);
            }
        }

        // ------------------------------------------------------------------
        // chain invariants
        // ------------------------------------------------------------------

        [[nodiscard]] bool checkChainInvariants_(const ImageAsset &asset) noexcept {
            if (asset.m_dimension != ImageDimension::image2d or asset.m_is_block or asset.m_tag != BlockTag::none) {
                return false;
            }
            if (asset.m_subresources.size() != asset.m_mip_count) {
                return false;
            }
            u64 offset = 0u;
            const mem::SharedBufferView storage = asset.m_storage.getBufferData();
            for (u32 level = 0u; level < asset.m_mip_count; ++level) {
                const u32 level_w = mipExtentAt(asset.m_width, level);
                const u32 level_h = mipExtentAt(asset.m_height, level);
                const ImageSubresource &sub = asset.m_subresources[level];
                if (sub.m_row_pitch != rowPitchFor(level_w, BlockTag::none)) {
                    return false;
                }
                if (sub.m_slice_pitch != slicePitchFor(level_w, level_h, BlockTag::none)) {
                    return false;
                }
                const mem::SharedBufferView view = sub.m_view.getBufferData();
                if (view.size() != static_cast<std::size_t>(sub.m_slice_pitch)) {
                    return false;
                }
                if (view.data() != storage.data() + offset) {
                    return false;
                }
                offset += sub.m_slice_pitch;
            }
            return offset == storage.size();
        }
    } // namespace

    // ------------------------------------------------------------------
    // mip chain generation
    // ------------------------------------------------------------------

    [[nodiscard]] std::error_code generateMipChain(ImageAsset &asset, MipGenDesc desc) {
        if (asset.m_dimension != ImageDimension::image2d or asset.m_width == 0u or
            asset.m_height == 0u) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }
        // KTX2/DDS-embedded chains pass through untouched: blocked assets never
        // decode to recompress, so generation rejects them here.
        if (asset.m_is_block or asset.m_tag != BlockTag::none or isBlocked(asset.m_format)) [[unlikely]] {
            return make_error_code(errc::function_not_supported);
        }
        if (asset.m_mip_count != 1u or asset.m_subresources.size() != 1u) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }
        if (not asset.m_storage.isValid() or not asset.m_storage.isMaterialized()) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }
        if (desc.m_preserve_coverage and
            (not(desc.m_alpha_cutoff > 0.0f) or not(desc.m_alpha_cutoff <= 1.0f))) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }
        const u32 count = mipCountFor(asset.m_width, asset.m_height);
        if (count <= 1u) {
            return default_value_v;
        }
        // Chain bytes count against the decode cap: a max-size single level has
        // no headroom left and fails closed here.
        u64 chain_bytes = 0u;
        for (u32 level = 0u; level < count; ++level) {
            chain_bytes += slicePitchFor(mipExtentAt(asset.m_width, level), mipExtentAt(asset.m_height, level), BlockTag::none);
        }
        if (chain_bytes > desc.m_limits.m_max_decoded_bytes) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }
        const mem::SharedBufferView top_bytes = asset.m_subresources.front().m_view.getBufferData();
        const auto top_size = static_cast<std::size_t>(slicePitchFor(asset.m_width, asset.m_height, BlockTag::none));
        if (top_bytes.size() != top_size) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }

        const bool to_linear = asset.m_is_srgb;
        const std::size_t top_count = static_cast<std::size_t>(asset.m_width) * asset.m_height;
        std::vector<float> top(top_count * 4u);
        for (std::size_t i = 0u; i < top_count; ++i) {
            const float r = byteToUnit_(top_bytes[i * 4u + 0u]);
            const float g = byteToUnit_(top_bytes[i * 4u + 1u]);
            const float b = byteToUnit_(top_bytes[i * 4u + 2u]);
            top[i * 4u + 0u] = to_linear ? mango::math::srgb_to_linear(r) : r;
            top[i * 4u + 1u] = to_linear ? mango::math::srgb_to_linear(g) : g;
            top[i * 4u + 2u] = to_linear ? mango::math::srgb_to_linear(b) : b;
            top[i * 4u + 3u] = byteToUnit_(top_bytes[i * 4u + 3u]);
        }
        const float target_coverage = desc.m_preserve_coverage ? coverageOf_(top, desc.m_alpha_cutoff) : 0.0f;

        mem::UniqueBuffer job = mem::UniqueBuffer::allocate(static_cast<std::size_t>(chain_bytes));
        if (const std::error_code err = job.materialize()) [[unlikely]] {
            return err;
        }
        if (not job.isMaterialized()) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }

        Expected<mem::MutableBufferView> chain = job.getMutableData();
        if (not chain.has_value() or chain->size() != static_cast<std::size_t>(chain_bytes)) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }

        std::ranges::copy(top_bytes.first(top_size), chain->begin());
        u64 offset = static_cast<u64>(top_size);

        std::vector<float> previous{};
        std::vector<float> work{};
        std::vector<float> next{};
        u32 previous_w = 0u;
        u32 previous_h = 0u;
        for (u32 level = 1u; level < count; ++level) {
            const u32 level_w = mipExtentAt(asset.m_width, level);
            const u32 level_h = mipExtentAt(asset.m_height, level);
            u32 source_w = 0u;
            u32 source_h = 0u;
            if (desc.m_high_quality) {
                work = top;
                source_w = asset.m_width;
                source_h = asset.m_height;
            } else {
                work = previous_w == 0u ? top : previous;
                source_w = previous_w == 0u ? asset.m_width : previous_w;
                source_h = previous_h == 0u ? asset.m_height : previous_h;
            }
            expandTransparentColors_(work, source_w, source_h);
            const float *const source = work.data();
            next.assign(static_cast<std::size_t>(level_w) * level_h * 4u, 0.0f);
            const int source_w_int = static_cast<int>(source_w);
            const int source_h_int = static_cast<int>(source_h);
            const int level_w_int = static_cast<int>(level_w);
            const int level_h_int = static_cast<int>(level_h);
            if (stbir_resize_float_linear(source, source_w_int, source_h_int, 0, next.data(), level_w_int, level_h_int, 0,
                    STBIR_RGBA) == nullptr) [[unlikely]] {
                return make_error_code(errc::invalid_argument);
            }
            if (desc.m_preserve_coverage) {
                preserveCoverage_(next, target_coverage, desc.m_alpha_cutoff);
            }
            std::byte *const dest = chain->data() + offset;
            for (std::size_t i = 0u; i < static_cast<std::size_t>(level_w) * level_h; ++i) {
                const float r = to_linear ? mango::math::linear_to_srgb(next[i * 4u + 0u]) : next[i * 4u + 0u];
                const float g = to_linear ? mango::math::linear_to_srgb(next[i * 4u + 1u]) : next[i * 4u + 1u];
                const float b = to_linear ? mango::math::linear_to_srgb(next[i * 4u + 2u]) : next[i * 4u + 2u];
                dest[i * 4u + 0u] = unitToByte_(r);
                dest[i * 4u + 1u] = unitToByte_(g);
                dest[i * 4u + 2u] = unitToByte_(b);
                dest[i * 4u + 3u] = unitToByte_(next[i * 4u + 3u]);
            }
            offset += slicePitchFor(level_w, level_h, BlockTag::none);
            previous = next;
            previous_w = level_w;
            previous_h = level_h;
        }

        mem::SharedBuffer frozen{};
        if (const std::error_code err = job.moveToShared(&frozen)) [[unlikely]] {
            return err;
        }
        if (not frozen.isMaterialized() or
            frozen.getBufferData().size() != static_cast<std::size_t>(chain_bytes)) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }
        asset.m_storage = frozen;
        asset.m_mip_count = count;
        asset.m_subresources.clear();
        u64 sub_offset = 0u;
        for (u32 level = 0u; level < count; ++level) {
            const u32 level_w = mipExtentAt(asset.m_width, level);
            const u32 level_h = mipExtentAt(asset.m_height, level);
            const u64 row_pitch = rowPitchFor(level_w, BlockTag::none);
            const u64 slice_pitch = slicePitchFor(level_w, level_h, BlockTag::none);
            asset.m_subresources.push_back(ImageSubresource{
                .m_view = asset.m_storage.subspan(static_cast<std::size_t>(sub_offset), static_cast<std::size_t>(slice_pitch)),
                .m_row_pitch = row_pitch,
                .m_slice_pitch = slice_pitch,
            });
            sub_offset += slice_pitch;
        }

        PPR_LOG(Image, info, "generated mip chain");
        PPR_ASSERT(checkChainInvariants_(asset));
        if (not checkChainInvariants_(asset)) [[unlikely]] {
            return make_error_code(errc::invalid_argument);
        }
        return default_value_v;
    }
}
