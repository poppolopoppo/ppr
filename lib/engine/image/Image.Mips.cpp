module;

#include "pP/Macros.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

module engine.image;

import :types;
import :mips;
import engine.core;
import engine.math;

import std;

namespace pP::image {
    using pP::make_error_code;

    namespace {
        // ------------------------------------------------------------------
        // SIMD pixel transfer
        // ------------------------------------------------------------------

        // Custom image limits may require more than the scratch arena's u32 slab capacity.
        using FloatWorkspace = std::vector<float, mem::STL<float, mem::GPA>>;

        [[nodiscard]] float4 unpackPixel_(const std::byte *const source) noexcept {
            u32 packed{};
            std::memcpy(&packed, source, sizeof(packed));
            if constexpr (std::endian::native == std::endian::big) {
                packed = std::byteswap(packed);
            }

            return float4::unpack(packed);
        }

        void packPixel_(std::byte *const destination, const float4 &bytes) noexcept {
            u32 packed = bytes.pack();
            if constexpr (std::endian::native == std::endian::big) {
                packed = std::byteswap(packed);
            }

            std::memcpy(destination, &packed, sizeof(packed));
        }

        [[nodiscard]] float4 quantizePixel_(const float4 &rgba) noexcept {
            const float4 scaled = pP::clamp(rgba, 0.0f, 1.0f) * 255.0f;
            const float4 integral = pP::floor(scaled);

            // Compare the fraction directly: adding 0.5 can round a just-below
            // half value upward before floor. Integer packing then preserves lround.
            return pP::select(scaled - integral >= 0.5f, integral + 1.0f, integral);
        }

        [[nodiscard]] float4 decodePixel_(const std::byte *const source, const bool srgb) noexcept {
            const float4 rgba = unpackPixel_(source) / 255.0f;
            float4 result = srgb ? pP::srgb_to_linear(rgba) : rgba;
            result.w = rgba.w;
            return result;
        }

        [[nodiscard]] float4 encodePixel_(const float4 &rgba, const bool srgb) noexcept {
            float4 result = srgb ? pP::linear_to_srgb(rgba) : rgba;
            result.w = rgba.w;
            return quantizePixel_(result);
        }

        [[nodiscard]] float4 normalizeNormal_(const float4 &rgba) noexcept {
            const float4 normal{rgba.xyz, 0.0f};
            const float length_squared = pP::dot(normal, normal);
            if (length_squared <= 1.0e-12f or normal.z <= 0.0f) {
                return float4{0.0f, 0.0f, 1.0f, 1.0f};
            }

            float4 result = normal / pP::sqrt(float4{length_squared});
            result.w = 1.0f;
            return result;
        }

        [[nodiscard]] std::error_code encodeLevel_(
            const std::span<float> rgba, std::byte *const destination, const bool srgb,
            const bool normal_map) noexcept {
            constexpr std::size_t scanline_pitch_in_pixels = 128u * 128u;
            return parallel::forRange(
                0u, rgba.size() / 4u,
                [&](std::size_t index) {
                    const std::size_t i = index * 4u;
                    float4 pixel = float4::uload(rgba.data() + i);

                    if (normal_map) {
                        pixel = normalizeNormal_(pixel);
                        float4::ustore(rgba.data() + i, pixel);

                        pixel = pixel * 0.5f + 0.5f;
                    }

                    packPixel_(destination + i, encodePixel_(pixel, srgb));
                },
                parallel::backgroundPool(), scanline_pitch_in_pixels);
        }

        // ------------------------------------------------------------------
        // SIMD alpha coverage
        // ------------------------------------------------------------------

        [[nodiscard]] float4 gatherAlphas_(const std::span<const float> rgba, const std::size_t pixel) noexcept {
            const std::size_t count = rgba.size() / 4u;
            // A padded final vector uses the same numerical path as full batches.
            return float4{
                rgba[pixel * 4u + 3u],
                pixel + 1u < count ? rgba[(pixel + 1u) * 4u + 3u] : 0.0f,
                pixel + 2u < count ? rgba[(pixel + 2u) * 4u + 3u] : 0.0f,
                pixel + 3u < count ? rgba[(pixel + 3u) * 4u + 3u] : 0.0f,
            };
        }

        [[nodiscard]] float coverageAtScale_(
            const std::span<const float> rgba, const float scale, const float cutoff) noexcept {
            std::size_t covered = 0u;
            const std::size_t count = rgba.size() / 4u;
            for (std::size_t i = 0u; i < count; i += 4u) {
                const float4 alpha = pP::min(gatherAlphas_(rgba, i) * scale, 1.0f);
                covered += std::popcount(pP::math::maskToInt(alpha >= cutoff));
            }

            return static_cast<float>(covered) / static_cast<float>(count);
        }

        // Upscale only, preserving the existing ceiling and 20 search steps.
        void preserveCoverage_(const std::span<float> rgba, const float target, const float cutoff) noexcept {
            if (coverageAtScale_(rgba, 1.0f, cutoff) >= target) {
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

            const std::size_t count = rgba.size() / 4u;
            for (std::size_t i = 0u; i < count; i += 4u) {
                const float4 alpha = pP::min(gatherAlphas_(rgba, i) * high, 1.0f);
                const std::size_t valid = (std::min) (std::size_t{4u}, count - i);
                for (std::size_t lane = 0u; lane < valid; ++lane) {
                    rgba[(i + lane) * 4u + 3u] = alpha[lane];
                }
            }
        }

        // ------------------------------------------------------------------
        // chain validation and publication
        // ------------------------------------------------------------------

        [[nodiscard]] bool validBaseExtent_(const ImageAsset &asset) noexcept {
            return asset.m_width != 0u and
                   asset.m_height != 0u and
                   asset.m_width % 4u == 0u and
                   asset.m_height % 4u == 0u and
                   static_cast<u64>(asset.m_width) * asset.m_height <= (std::numeric_limits<std::size_t>::max)() / 4u;
        }

        [[nodiscard]] bool checkChainInvariants_(const ImageAsset &asset) noexcept {
            if (asset.m_dimension != EImageDimension::image2d or
                asset.m_is_block or
                asset.m_tag != EBlockTag::none or
                isBlocked(asset.m_format) or
                asset.m_is_srgb != isSrgb(asset.m_format) or
                asset.m_block_w != 1u or
                asset.m_block_h != 1u or
                asset.m_bytes_per_block != 4u or
                not validBaseExtent_(asset) or
                asset.m_mip_count == 0u or
                asset.m_mip_count > static_cast<u32>(std::bit_width((std::max) (asset.m_width, asset.m_height))) or
                asset.m_subresources.size() != asset.m_mip_count or
                not asset.m_storage.isMaterialized()) {
                return false;
            }

            u64 offset = 0u;
            const mem::SharedBufferView storage = asset.m_storage.getBufferData();
            for (u32 level = 0u; level < asset.m_mip_count; ++level) {
                const u32 width = mipExtentAt(asset.m_width, level);
                const u32 height = mipExtentAt(asset.m_height, level);
                const ImageSubresource &sub = asset.m_subresources[level];
                const u64 size = slicePitchFor(width, height, EBlockTag::none);
                if (sub.m_row_pitch != rowPitchFor(width, EBlockTag::none) or
                    sub.m_slice_pitch != size or
                    offset > storage.size() or
                    size > storage.size() - offset) {
                    return false;
                }

                const mem::SharedBufferView view = sub.m_view.getBufferData();
                if (view.size() != size or view.data() != storage.data() + offset) {
                    return false;
                }

                offset += size;
            }

            return offset == storage.size();
        }

        [[nodiscard]] std::error_code publishChain_(
            ImageAsset &asset, mem::UniqueBuffer &job, const u32 count, const std::size_t bytes) {
            mem::SharedBuffer frozen{};
            PPR_RETURN_ERROR_ON_FAIL(Image, job.moveToShared(&frozen));
            if (not frozen.isMaterialized() or frozen.getBufferData().size() != bytes) {
                PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
            }

            Array<ImageSubresource> subresources{};
            subresources.reserve(count);
            std::size_t offset = 0u;
            for (u32 level = 0u; level < count; ++level) {
                const u32 width = mipExtentAt(asset.m_width, level);
                const u32 height = mipExtentAt(asset.m_height, level);
                const u64 size = slicePitchFor(width, height, EBlockTag::none);
                subresources.push_back(
                    ImageSubresource{
                        .m_view = frozen.subspan(offset, static_cast<std::size_t>(size)),
                        .m_row_pitch = rowPitchFor(width, EBlockTag::none),
                        .m_slice_pitch = size,
                    });

                offset += static_cast<std::size_t>(size);
            }

            // All fallible work finishes before replacing the caller's owners.
            asset.m_storage = std::move(frozen);
            asset.m_subresources = std::move(subresources);
            asset.m_mip_count = count;
            PPR_ASSERT(checkChainInvariants_(asset));
            return {};
        }

        // ------------------------------------------------------------------
        // whole-chain flooding workspace
        // ------------------------------------------------------------------

        struct FloodLevel {
            std::span<float> m_rgba{};
            u32 m_width{};
            u32 m_height{};
        };

        [[nodiscard]] float4 maskAt_(
            const mem::SharedBufferView bytes, const std::size_t pixel, const EImageChannel channel) noexcept {
            const float4 rgba = unpackPixel_(bytes.data() + pixel * 4u) / 255.0f;
            return float4{rgba[static_cast<u32>(channel)]};
        }

        void reduceSupport_(const FloodLevel &source, const FloodLevel &destination) noexcept {
            for (u32 y = 0u; y < destination.m_height; ++y) {
                const u32 begin_y = static_cast<u32>(static_cast<u64>(y) * source.m_height / destination.m_height);
                const u32 end_y = static_cast<u32>(static_cast<u64>(y + 1u) * source.m_height / destination.m_height);

                for (u32 x = 0u; x < destination.m_width; ++x) {
                    const u32 begin_x = static_cast<u32>(static_cast<u64>(x) * source.m_width / destination.m_width);
                    const u32 end_x = static_cast<u32>(static_cast<u64>(x + 1u) * source.m_width / destination.m_width);
                    float4 sum{0.0f};

                    for (u32 source_y = begin_y; source_y < end_y; ++source_y) {
                        for (u32 source_x = begin_x; source_x < end_x; ++source_x) {
                            const std::size_t at = (static_cast<std::size_t>(source_y) * source.m_width + source_x) *
                                                   4u;
                            sum += float4::uload(source.m_rgba.data() + at);
                        }
                    }

                    const float4 average = sum / static_cast<float>((end_x - begin_x) * (end_y - begin_y));
                    const std::size_t at = (static_cast<std::size_t>(y) * destination.m_width + x) * 4u;
                    float4::ustore(destination.m_rgba.data() + at, average);
                }
            }
        }

        void reduceMask_(
            const std::span<const float> source, const u32 source_width, const u32 source_height,
            const std::span<float> destination, const u32 width, const u32 height) noexcept {
            for (u32 y = 0u; y < height; ++y) {
                const u32 begin_y = static_cast<u32>(static_cast<u64>(y) * source_height / height);
                const u32 end_y = static_cast<u32>(static_cast<u64>(y + 1u) * source_height / height);

                for (u32 x = 0u; x < width; ++x) {
                    const u32 begin_x = static_cast<u32>(static_cast<u64>(x) * source_width / width);
                    const u32 end_x = static_cast<u32>(static_cast<u64>(x + 1u) * source_width / width);
                    std::array<float, 4> samples{};
                    std::size_t lane = 0u;
                    float4 sum{0.0f};

                    for (u32 source_y = begin_y; source_y < end_y; ++source_y) {
                        for (u32 source_x = begin_x; source_x < end_x; ++source_x) {
                            samples[lane++] = source[static_cast<std::size_t>(source_y) * source_width + source_x];
                            if (lane == samples.size()) {
                                sum += float4::uload(samples.data());
                                samples.fill(0.0f);
                                lane = 0u;
                            }
                        }
                    }

                    sum += float4::uload(samples.data());
                    sum /= static_cast<float>((end_x - begin_x) * (end_y - begin_y));
                    destination[static_cast<std::size_t>(y) * width + x] = pP::dot(sum, float4{1.0f});
                }
            }
        }

        [[nodiscard]] float4 sampleParent_(
            const FloodLevel &parent, const FloodLevel &child, const u32 x, const u32 y) noexcept {
            const float4 scale{static_cast<float>(parent.m_width), static_cast<float>(parent.m_height), 1.0f, 1.0f};
            const float4 extent{static_cast<float>(child.m_width), static_cast<float>(child.m_height), 1.0f, 1.0f};
            const float4 upper{
                static_cast<float>(parent.m_width - 1u), static_cast<float>(parent.m_height - 1u), 0.0f, 0.0f};
            const float4 coordinate = pP::clamp(
                (float4{static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f} + 0.5f) * scale / extent - 0.5f,
                float4{0.0f}, upper);
            const float4 lower = pP::floor(coordinate);
            const uint4 index = pP::truncToUInt<float, 4u>(lower);
            const float4 fraction = coordinate - lower;

            // Large extents can round their final float coordinate upward.
            // Parent extents are at most half a u32 base; conversion fits uint4.
            const u32 x0 = (std::min) (static_cast<u32>(index.x), parent.m_width - 1u);
            const u32 y0 = (std::min) (static_cast<u32>(index.y), parent.m_height - 1u);
            const u32 x1 = (std::min) (x0 + 1u, parent.m_width - 1u);
            const u32 y1 = (std::min) (y0 + 1u, parent.m_height - 1u);

            const auto load = [&](const u32 px, const u32 py) noexcept {
                return float4::uload(parent.m_rgba.data() + (static_cast<std::size_t>(py) * parent.m_width + px) * 4u);
            };

            return pP::lerp(
                pP::lerp(load(x0, y0), load(x1, y0), fraction.x), pP::lerp(load(x0, y1), load(x1, y1), fraction.x),
                fraction.y);
        }
    }

    // ------------------------------------------------------------------
    // mip chain generation
    // ------------------------------------------------------------------

    [[nodiscard]] std::error_code generateMipChain(ImageAsset &asset, const MipGenDesc desc) {
        if (asset.m_dimension != EImageDimension::image2d or not validBaseExtent_(asset)) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        if (asset.m_is_block or asset.m_tag != EBlockTag::none or isBlocked(asset.m_format)) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::function_not_supported);
        }

        if (asset.m_mip_count != 1u or not checkChainInvariants_(asset)) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        if (desc.m_has_preserve_coverage and MipGenDesc::alpha_cutoff_domain.validate(desc.m_alpha_cutoff)) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        if (asset.m_width > static_cast<u32>((std::numeric_limits<int>::max)()) or
            asset.m_height > static_cast<u32>((std::numeric_limits<int>::max)()) or
            (desc.m_normal_map and (asset.m_is_srgb or desc.m_has_preserve_coverage))) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        const u32 count = desc.m_full_chain ? std::bit_width((std::max) (asset.m_width, asset.m_height))
                                            : mipCountFor(asset.m_width, asset.m_height);
        if (count <= 1u and not desc.m_normal_map) {
            return {};
        }

        u64 chain_bytes = 0u;
        for (u32 level = 0u; level < count; ++level) {
            const u64 size = slicePitchFor(
                mipExtentAt(asset.m_width, level), mipExtentAt(asset.m_height, level), EBlockTag::none);
            if (size > (std::min) (desc.m_limits.m_max_decoded_bytes,
                           static_cast<u64>((std::numeric_limits<std::size_t>::max)())) -
                           chain_bytes) {
                PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
            }

            chain_bytes += size;
        }

        const mem::SharedBufferView top_bytes = asset.m_subresources.front().m_view.getBufferData();
        if (top_bytes.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(float)) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        FloatWorkspace top(top_bytes.size());
        for (std::size_t i = 0u; i < top.size(); i += 4u) {
            float4 pixel = decodePixel_(top_bytes.data() + i, asset.m_is_srgb);
            if (desc.m_normal_map) {
                pixel = pixel * 2.0f - 1.0f;
            }

            float4::ustore(top.data() + i, pixel);
        }

        const float target_coverage = desc.m_has_preserve_coverage ? coverageAtScale_(top, 1.0f, desc.m_alpha_cutoff)
                                                                   : 0.0f;
        mem::UniqueBuffer job = mem::UniqueBuffer::allocate(static_cast<std::size_t>(chain_bytes));
        PPR_RETURN_ERROR_ON_FAIL(Image, job.materialize());
        Expected<mem::MutableBufferView> chain = job.getMutableData();
        if (not chain.has_value() or chain->size() != chain_bytes) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        std::ranges::copy(top_bytes, chain->begin());
        if (desc.m_normal_map) {
            PPR_RETURN_ERROR_ON_FAIL(Image, encodeLevel_(top, chain->data(), false, true));
        }

        FloatWorkspace previous{};
        FloatWorkspace next{};
        std::size_t offset = top_bytes.size();
        for (u32 level = 1u; level < count; ++level) {
            const u32 width = mipExtentAt(asset.m_width, level);
            const u32 height = mipExtentAt(asset.m_height, level);
            const u32 source_level = desc.m_is_high_quality ? 0u : level - 1u;
            const u32 source_width = mipExtentAt(asset.m_width, source_level);
            const u32 source_height = mipExtentAt(asset.m_height, source_level);
            const float *const source = source_level == 0u ? top.data() : previous.data();
            next.resize(static_cast<std::size_t>(width) * height * 4u);

            {
                const hal::ProfileScope resize_scope{"Image.Mips.Resize"};

                if (stbir_resize_float_linear(
                        source, static_cast<int>(source_width), static_cast<int>(source_height), 0, next.data(),
                        static_cast<int>(width), static_cast<int>(height), 0, STBIR_RGBA) == nullptr) {
                    // Validated storage/dimensions leave stb's scratch allocation as its failure.
                    throw std::bad_alloc{};
                }
            }

            if (desc.m_has_preserve_coverage) {
                preserveCoverage_(next, target_coverage, desc.m_alpha_cutoff);
            }

            PPR_RETURN_ERROR_ON_FAIL(
                Image, encodeLevel_(next, chain->data() + offset, asset.m_is_srgb, desc.m_normal_map));
            offset += static_cast<std::size_t>(slicePitchFor(width, height, EBlockTag::none));
            previous.swap(next);
        }

        PPR_RETURN_ERROR_ON_FAIL(Image, publishChain_(asset, job, count, static_cast<std::size_t>(chain_bytes)));
        PPR_LOG(Image, info, "generated mip chain");
        return {};
    }

    // ------------------------------------------------------------------
    // explicit whole-chain color flooding
    // ------------------------------------------------------------------

    [[nodiscard]] std::error_code floodMipChain(ImageAsset &color, const MipFloodDesc desc) {
        const ImageAsset &alpha = desc.m_alpha_source != nullptr ? *desc.m_alpha_source : color;
        if (color.m_is_block or isBlocked(color.m_format) or alpha.m_is_block or isBlocked(alpha.m_format)) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::function_not_supported);
        }

        if (not checkChainInvariants_(color) or
            not checkChainInvariants_(alpha) or
            color.m_width != alpha.m_width or
            color.m_height != alpha.m_height or
            (alpha.m_mip_count != 1u and alpha.m_mip_count < color.m_mip_count) or
            static_cast<u32>(desc.m_alpha_channel) > static_cast<u32>(EImageChannel::alpha) or
            MipGenDesc::alpha_cutoff_domain.validate(desc.m_alpha_cutoff) or
            desc.m_is_premultiplied_alpha) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        const u32 support_count = std::bit_width((std::max) (color.m_width, color.m_height));
        std::array<FloodLevel, std::numeric_limits<u32>::digits> levels{};
        std::array<std::span<float>, std::numeric_limits<u32>::digits> masks{};
        std::size_t support_floats = 0u;
        std::size_t mask_floats = 0u;
        const bool derive_masks = alpha.m_mip_count == 1u and color.m_mip_count > 1u;
        for (u32 level = 0u; level < support_count; ++level) {
            const u32 width = mipExtentAt(color.m_width, level);
            const u32 height = mipExtentAt(color.m_height, level);
            const u64 pixels = static_cast<u64>(width) * height;
            if (pixels > ((std::numeric_limits<std::size_t>::max)() / sizeof(float) - support_floats) / 4u) {
                PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
            }

            support_floats += static_cast<std::size_t>(pixels) * 4u;
            if (derive_masks and level < color.m_mip_count) {
                mask_floats += static_cast<std::size_t>(pixels);
            }
        }

        FloatWorkspace support(support_floats);
        FloatWorkspace mask_storage(mask_floats);
        std::size_t support_offset = 0u;
        std::size_t mask_offset = 0u;
        for (u32 level = 0u; level < support_count; ++level) {
            const u32 width = mipExtentAt(color.m_width, level);
            const u32 height = mipExtentAt(color.m_height, level);
            const std::size_t pixels = static_cast<std::size_t>(width) * height;
            levels[level] = FloodLevel{std::span<float>{support}.subspan(support_offset, pixels * 4u), width, height};
            support_offset += pixels * 4u;

            if (derive_masks and level < color.m_mip_count) {
                masks[level] = std::span<float>{mask_storage}.subspan(mask_offset, pixels);
                mask_offset += pixels;
            }
        }

        const mem::SharedBufferView top = color.m_subresources.front().m_view.getBufferData();
        const mem::SharedBufferView top_mask = alpha.m_subresources.front().m_view.getBufferData();
        for (std::size_t pixel = 0u; pixel < top.size() / 4u; ++pixel) {
            const float4 mask = maskAt_(top_mask, pixel, desc.m_alpha_channel);
            float4 seed = decodePixel_(top.data() + pixel * 4u, color.m_is_srgb);
            seed.w = 1.0f;
            seed = pP::select(mask >= desc.m_alpha_cutoff, seed, float4{0.0f});
            float4::ustore(levels[0u].m_rgba.data() + pixel * 4u, seed);

            if (derive_masks) {
                masks[0u][pixel] = mask.x;
            }
        }

        // Confidence is seed coverage, independent of original/filtered alpha.
        // Integer box partitions include odd final rows/columns and thin axes.
        for (u32 level = 1u; level < support_count; ++level) {
            reduceSupport_(levels[level - 1u], levels[level]);

            if (derive_masks and level < color.m_mip_count) {
                reduceMask_(
                    masks[level - 1u], levels[level - 1u].m_width, levels[level - 1u].m_height, masks[level],
                    levels[level].m_width, levels[level].m_height);
            }
        }

        FloodLevel &coarsest = levels[support_count - 1u];
        const float4 coarse = float4::uload(coarsest.m_rgba.data());
        const float4 weight = coarse.wwww;
        const float4 safe_weight = pP::select(weight > 0.0f, weight, float4{1.0f});
        float4 filled = coarse / safe_weight;
        filled.w = 1.0f;
        float4::ustore(coarsest.m_rgba.data(), filled);

        for (u32 level = support_count - 1u; level > 0u; --level) {
            FloodLevel &child = levels[level - 1u];
            const FloodLevel &parent = levels[level];

            for (u32 y = 0u; y < child.m_height; ++y) {
                for (u32 x = 0u; x < child.m_width; ++x) {
                    const std::size_t at = (static_cast<std::size_t>(y) * child.m_width + x) * 4u;
                    const float4 local = float4::uload(child.m_rgba.data() + at);
                    float4 result = local + (float4{1.0f} - local.wwww) * sampleParent_(parent, child, x, y);
                    result.w = 1.0f;
                    float4::ustore(child.m_rgba.data() + at, result);
                }
            }
        }

        const mem::SharedBufferView original = color.m_storage.getBufferData();
        mem::UniqueBuffer job = mem::UniqueBuffer::allocate(original.size());
        PPR_RETURN_ERROR_ON_FAIL(Image, job.materialize());
        Expected<mem::MutableBufferView> destination = job.getMutableData();
        if (not destination.has_value() or destination->size() != original.size()) {
            PPR_RETURN_ERROR_ON_FAIL(Image, errc::invalid_argument);
        }

        std::ranges::copy(original, destination->begin());
        std::size_t offset = 0u;
        for (u32 level = 0u; level < color.m_mip_count; ++level) {
            const FloodLevel &source = levels[level];
            const mem::SharedBufferView mask_bytes = derive_masks ? mem::SharedBufferView{}
                                                                  : alpha.m_subresources[level].m_view.getBufferData();

            for (std::size_t pixel = 0u; pixel < source.m_rgba.size() / 4u; ++pixel) {
                const float4 mask = derive_masks ? float4{masks[level][pixel]}
                                                 : maskAt_(mask_bytes, pixel, desc.m_alpha_channel);
                std::byte *const at = destination->data() + offset + pixel * 4u;
                const float4 before = unpackPixel_(at);
                const float4 replacement = encodePixel_(
                    float4::uload(source.m_rgba.data() + pixel * 4u), color.m_is_srgb);
                float4 after = pP::select(mask < desc.m_alpha_cutoff, replacement, before);
                after.w = before.w;
                packPixel_(at, after);
            }

            offset += source.m_rgba.size();
        }

        PPR_RETURN_ERROR_ON_FAIL(Image, publishChain_(color, job, color.m_mip_count, original.size()));
        return {};
    }
}
