module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.image;
import std;

// Coverage preserve: mask-material alpha coverage survives the
// down-chain via upscale-only binary search with abort-below-1 semantics.
// One focused test per TU.
namespace pP::tests::detail {
    namespace MipsCoverage {
        [[nodiscard]] image::ImageAsset makeSinglePixel_(const u32 size) {
            const std::size_t bytes = static_cast<std::size_t>(size) * size * 4u;
            mem::UniqueBuffer job = mem::UniqueBuffer::allocate(bytes);
            if (job.materialize() or not job.isMaterialized()) {
                return {};
            }
            Expected<mem::MutableBufferView> view = job.getMutableData();
            if (not view.has_value() or view->size() != bytes) {
                return {};
            }
            std::ranges::fill(*view, std::byte{0x00});
            // One opaque white pixel dilutes below a 0.5 cutoff at the first
            // halving, so the search path (not the abort path) must engage.
            const std::size_t at = (static_cast<std::size_t>(5u) * size + 7u) * 4u;
            (*view)[at + 0u] = std::byte{0xFF};
            (*view)[at + 1u] = std::byte{0xFF};
            (*view)[at + 2u] = std::byte{0xFF};
            (*view)[at + 3u] = std::byte{0xFF};
            mem::SharedBuffer frozen{};
            if (job.moveToShared(&frozen) or not frozen.isMaterialized()) {
                return {};
            }
            image::ImageAsset built{};
            built.m_width = size;
            built.m_height = size;
            built.m_mip_count = 1u;
            built.m_dimension = image::ImageDimension::image2d;
            built.m_format = image::NativeImageFormat::rgba8_linear;
            built.m_tag = image::BlockTag::none;
            built.m_block_w = 1u;
            built.m_block_h = 1u;
            built.m_bytes_per_block = 4u;
            built.m_is_srgb = false;
            built.m_is_block = false;
            built.m_storage = frozen;
            built.m_subresources.push_back(image::ImageSubresource{
                .m_view = built.m_storage.subspan(0u, bytes),
                .m_row_pitch = static_cast<u64>(size) * 4u,
                .m_slice_pitch = static_cast<u64>(bytes),
            });
            return built;
        }

        [[nodiscard]] image::ImageAsset makeHalfOpaque_(const u32 size) {
            const std::size_t bytes = static_cast<std::size_t>(size) * size * 4u;
            mem::UniqueBuffer job = mem::UniqueBuffer::allocate(bytes);
            if (job.materialize() or not job.isMaterialized()) {
                return {};
            }
            Expected<mem::MutableBufferView> view = job.getMutableData();
            if (not view.has_value() or view->size() != bytes) {
                return {};
            }
            for (u32 y = 0u; y < size; ++y) {
                for (u32 x = 0u; x < size; ++x) {
                    const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4u;
                    const std::byte alpha = x < size / 2u ? std::byte{0xFF} : std::byte{0x00};
                    (*view)[at + 0u] = std::byte{0xFF};
                    (*view)[at + 1u] = std::byte{0xFF};
                    (*view)[at + 2u] = std::byte{0xFF};
                    (*view)[at + 3u] = alpha;
                }
            }
            mem::SharedBuffer frozen{};
            if (job.moveToShared(&frozen) or not frozen.isMaterialized()) {
                return {};
            }
            image::ImageAsset built{};
            built.m_width = size;
            built.m_height = size;
            built.m_mip_count = 1u;
            built.m_dimension = image::ImageDimension::image2d;
            built.m_format = image::NativeImageFormat::rgba8_linear;
            built.m_tag = image::BlockTag::none;
            built.m_block_w = 1u;
            built.m_block_h = 1u;
            built.m_bytes_per_block = 4u;
            built.m_is_srgb = false;
            built.m_is_block = false;
            built.m_storage = frozen;
            built.m_subresources.push_back(image::ImageSubresource{
                .m_view = built.m_storage.subspan(0u, bytes),
                .m_row_pitch = static_cast<u64>(size) * 4u,
                .m_slice_pitch = static_cast<u64>(bytes),
            });
            return built;
        }

        [[nodiscard]] float coverageOf_(const image::ImageSubresource &sub) noexcept {
            const mem::SharedBufferView pixels = sub.m_view.getBufferData();
            std::size_t covered = 0u;
            const std::size_t count = pixels.size() / 4u;
            for (std::size_t i = 0u; i < count; ++i) {
                if (std::to_integer<unsigned int>(pixels[i * 4u + 3u]) >= 128u) {
                    ++covered;
                }
            }
            return static_cast<float>(covered) / static_cast<float>(count);
        }

        PPR_UNIT_TEST (mips_coverage_preserved) {
            constexpr u32 kSize = 32u;
            image::MipGenDesc preserve{};
            preserve.m_preserve_coverage = true;
            preserve.m_alpha_cutoff = 0.5f;

            // Without preserve the lone pixel dilutes to zero coverage at mip 1,
            // proving the fixture exercises the search rather than the abort.
            image::ImageAsset plain = makeSinglePixel_(kSize);
            PPR_TEST_ASSERT(plain.m_width == kSize);
            PPR_TEST_ASSERT(not image::generateMipChain(plain, image::MipGenDesc{}));
            PPR_TEST_ASSERT(plain.m_mip_count == 4u);
            PPR_TEST_ASSERT(coverageOf_(plain.m_subresources[0]) == 1.0f / 1024.0f);
            PPR_TEST_ASSERT(coverageOf_(plain.m_subresources[1]) == 0.0f);

            // With preserve every down-chain level keeps at least top coverage.
            image::ImageAsset kept = makeSinglePixel_(kSize);
            PPR_TEST_ASSERT(not image::generateMipChain(kept, preserve));
            PPR_TEST_ASSERT(kept.m_mip_count == 4u);
            const float target = coverageOf_(kept.m_subresources[0]);
            PPR_TEST_ASSERT(target == 1.0f / 1024.0f);
            for (u32 level = 1u; level < kept.m_mip_count; ++level) {
                PPR_TEST_ASSERT(coverageOf_(kept.m_subresources[level]) >= target);
            }

            // Abort-below-1: a stable half-opaque field needs no upscale, so the
            // preserve run must be byte-identical to the plain run.
            image::ImageAsset stable_plain = makeHalfOpaque_(16u);
            image::ImageAsset stable_kept = makeHalfOpaque_(16u);
            PPR_TEST_ASSERT(not image::generateMipChain(stable_plain, image::MipGenDesc{}));
            PPR_TEST_ASSERT(not image::generateMipChain(stable_kept, preserve));
            PPR_TEST_ASSERT(stable_plain.m_mip_count == stable_kept.m_mip_count);
            PPR_TEST_ASSERT(std::ranges::equal(
                stable_plain.m_storage.getBufferData(), stable_kept.m_storage.getBufferData()));
        };
    } // namespace MipsCoverage
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest mips_coverage = UnitTest::Named("mips_coverage") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::MipsCoverage::mips_coverage_preserved,
        });
    };

    const UnitTest &mipsCoverageTests() noexcept {
        return mips_coverage;
    }
} // namespace pP::tests
