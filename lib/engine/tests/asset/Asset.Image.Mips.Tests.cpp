module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.image;
import std;

// Mip goldens (CPU-only): chain rules, coverage preservation, edge
// anti-bleed, HQ equivalence bounds. One flat group: asset/image/mips
// (nested under the image root in Asset.Image.Tests.cpp).
namespace pP::tests::detail {
    namespace MipsChain {
        [[nodiscard]] image::ImageAsset makeSolid_(const u32 width, const u32 height, const std::byte fill) {
            const std::size_t size = static_cast<std::size_t>(width) * height * 4u;
            mem::UniqueBuffer job = mem::UniqueBuffer::allocate(size);
            if (job.materialize() or not job.isMaterialized()) {
                return {};
            }
            Expected<mem::MutableBufferView> view = job.getMutableData();
            if (not view.has_value() or view->size() != size) {
                return {};
            }

            std::ranges::fill(*view, fill);

            mem::SharedBuffer frozen{};
            if (job.moveToShared(&frozen) or not frozen.isMaterialized()) {
                return {};
            }

            image::ImageAsset built{};
            built.m_width = width;
            built.m_height = height;
            built.m_mip_count = 1u;
            built.m_dimension = image::EImageDimension::image2d;
            built.m_format = image::ENativeImageFormat::rgba8_srgb;
            built.m_tag = image::EBlockTag::none;
            built.m_block_w = 1u;
            built.m_block_h = 1u;
            built.m_bytes_per_block = 4u;
            built.m_is_srgb = true;
            built.m_is_block = false;
            built.m_storage = frozen;
            built.m_subresources.push_back(image::ImageSubresource{
                .m_view = built.m_storage.subspan(0u, size),
                .m_row_pitch = static_cast<u64>(width) * 4u,
                .m_slice_pitch = static_cast<u64>(size),
            });
            return built;
        }

        PPR_UNIT_TEST (mips_chain_rules) {
            PPR_TEST_ASSERT(image::mipCountFor(16u, 16u) == 3u);
            PPR_TEST_ASSERT(image::mipCountFor(8u, 8u) == 2u);
            PPR_TEST_ASSERT(image::mipCountFor(1024u, 512u) == 8u);
            PPR_TEST_ASSERT(image::mipCountFor(4u, 4u) == 1u);
            PPR_TEST_ASSERT(image::mipCountFor(6u, 4u) == 1u);
            PPR_TEST_ASSERT(image::mipCountFor(2u, 2u) == 1u);
            PPR_TEST_ASSERT(image::mipCountFor(1u, 1u) == 1u);
            PPR_TEST_ASSERT(image::mipExtentAt(16u, 0u) == 16u);
            PPR_TEST_ASSERT(image::mipExtentAt(16u, 2u) == 4u);
            PPR_TEST_ASSERT(image::mipExtentAt(5u, 3u) == 1u);

            image::ImageAsset chained = makeSolid_(16u, 16u, std::byte{0x7F});
            PPR_TEST_ASSERT(chained.m_width == 16u);
            // Copy: generation replaces the frozen storage, so no view may be
            // held across the call.
            const std::vector<std::byte> top_before(
                chained.m_subresources.front().m_view.getBufferData().begin(),
                chained.m_subresources.front().m_view.getBufferData().end());
            PPR_TEST_ASSERT(top_before.size() == 1024u);

            PPR_TEST_ASSERT(not image::generateMipChain(chained, image::MipGenDesc{}));
            PPR_TEST_ASSERT(chained.m_mip_count == 3u);
            PPR_TEST_ASSERT(chained.m_subresources.size() == 3u);
            PPR_TEST_ASSERT(chained.m_format == image::ENativeImageFormat::rgba8_srgb);
            PPR_TEST_ASSERT(chained.m_is_srgb);
            PPR_TEST_ASSERT(not chained.m_is_block);

            // Level 0 is preserved verbatim; levels halve to 4 px.
            const mem::SharedBufferView top_after = chained.m_subresources[0].m_view.getBufferData();
            PPR_TEST_ASSERT(top_after.size() == top_before.size());
            PPR_TEST_ASSERT(std::ranges::equal(top_after, top_before));
            PPR_TEST_ASSERT(chained.m_subresources[0].m_row_pitch == 64u);
            PPR_TEST_ASSERT(chained.m_subresources[0].m_slice_pitch == 1024u);
            PPR_TEST_ASSERT(chained.m_subresources[1].m_row_pitch == 32u);
            PPR_TEST_ASSERT(chained.m_subresources[1].m_slice_pitch == 256u);
            PPR_TEST_ASSERT(chained.m_subresources[2].m_row_pitch == 16u);
            PPR_TEST_ASSERT(chained.m_subresources[2].m_slice_pitch == 64u);

            // One frozen storage holds the whole chain contiguously.
            PPR_TEST_ASSERT(chained.m_storage.isMaterialized());
            PPR_TEST_ASSERT(chained.m_storage.getBufferData().size() == 1344u);

            // Solid input filters to solid levels.
            const mem::SharedBufferView tail = chained.m_subresources[2].m_view.getBufferData();
            PPR_TEST_ASSERT(static_cast<unsigned char>(tail[0]) == 0x7Fu);
            PPR_TEST_ASSERT(static_cast<unsigned char>(tail[1]) == 0x7Fu);

            // A second generation pass over a chained asset fails closed.
            const std::error_code twice = image::generateMipChain(chained, image::MipGenDesc{});
            PPR_TEST_ASSERT(twice == image::make_error_code(image::errc::invalid_argument));
            PPR_TEST_ASSERT(chained.m_mip_count == 3u);

            // Tiny assets stay single-level (successful no-op).
            image::ImageAsset tiny = makeSolid_(2u, 2u, std::byte{0xFF});
            PPR_TEST_ASSERT(tiny.m_width == 2u);
            PPR_TEST_ASSERT(not image::generateMipChain(tiny, image::MipGenDesc{}));
            PPR_TEST_ASSERT(tiny.m_mip_count == 1u);
            PPR_TEST_ASSERT(tiny.m_subresources.size() == 1u);

            // Chain bytes count against the decode cap.
            image::ImageAsset capped = makeSolid_(16u, 16u, std::byte{0x11});
            image::MipGenDesc tight{};
            tight.m_limits.m_max_decoded_bytes = 100u;
            const std::error_code over = image::generateMipChain(capped, tight);
            PPR_TEST_ASSERT(over == image::make_error_code(image::errc::invalid_argument));
            PPR_TEST_ASSERT(capped.m_mip_count == 1u);
        };
    } // namespace MipsChain
} // namespace pP::tests::detail

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
            built.m_dimension = image::EImageDimension::image2d;
            built.m_format = image::ENativeImageFormat::rgba8_linear;
            built.m_tag = image::EBlockTag::none;
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
            built.m_dimension = image::EImageDimension::image2d;
            built.m_format = image::ENativeImageFormat::rgba8_linear;
            built.m_tag = image::EBlockTag::none;
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
            preserve.m_has_preserve_coverage = true;
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

namespace pP::tests::detail {
    namespace MipsBleed {
        [[nodiscard]] image::ImageAsset makeRedOnTransparent_() {
            constexpr u32 kSize = 8u;
            constexpr std::size_t kBytes = kSize * kSize * 4u;
            mem::UniqueBuffer job = mem::UniqueBuffer::allocate(kBytes);
            if (job.materialize() or not job.isMaterialized()) {
                return {};
            }
            Expected<mem::MutableBufferView> view = job.getMutableData();
            if (not view.has_value() or view->size() != kBytes) {
                return {};
            }

            std::ranges::fill(*view, std::byte{0x00});
            // 3x3 opaque red block at the origin: mip texels straddling its
            // border are the bleed witnesses (partial alpha, must stay red).
            for (u32 y = 0u; y < 3u; ++y) {
                for (u32 x = 0u; x < 3u; ++x) {
                    const std::size_t at = (static_cast<std::size_t>(y) * kSize + x) * 4u;
                    (*view)[at + 0u] = std::byte{0xFF};
                    (*view)[at + 1u] = std::byte{0x00};
                    (*view)[at + 2u] = std::byte{0x00};
                    (*view)[at + 3u] = std::byte{0xFF};
                }
            }

            mem::SharedBuffer frozen{};
            if (job.moveToShared(&frozen) or not frozen.isMaterialized()) {
                return {};
            }

            image::ImageAsset built{};
            built.m_width = kSize;
            built.m_height = kSize;
            built.m_mip_count = 1u;
            built.m_dimension = image::EImageDimension::image2d;
            built.m_format = image::ENativeImageFormat::rgba8_srgb;
            built.m_tag = image::EBlockTag::none;
            built.m_block_w = 1u;
            built.m_block_h = 1u;
            built.m_bytes_per_block = 4u;
            built.m_is_srgb = true;
            built.m_is_block = false;
            built.m_storage = frozen;
            built.m_subresources.push_back(image::ImageSubresource{
                .m_view = built.m_storage.subspan(0u, kBytes),
                .m_row_pitch = static_cast<u64>(kSize) * 4u,
                .m_slice_pitch = static_cast<u64>(kBytes),
            });
            return built;
        }

        [[nodiscard]] std::array<unsigned int, 4> texelOf_(
            const image::ImageSubresource &sub, const u32 x, const u32 y, const u32 width) noexcept {
            const mem::SharedBufferView pixels = sub.m_view.getBufferData();
            const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4u;
            return {
                std::to_integer<unsigned int>(pixels[at + 0u]),
                std::to_integer<unsigned int>(pixels[at + 1u]),
                std::to_integer<unsigned int>(pixels[at + 2u]),
                std::to_integer<unsigned int>(pixels[at + 3u]),
            };
        }

        PPR_UNIT_TEST (mips_no_bleed_at_edges) {
            image::ImageAsset chained = makeRedOnTransparent_();
            PPR_TEST_ASSERT(chained.m_width == 8u);
            PPR_TEST_ASSERT(not image::generateMipChain(chained, image::MipGenDesc{}));
            PPR_TEST_ASSERT(chained.m_mip_count == 2u);
            PPR_TEST_ASSERT(chained.m_subresources.size() == 2u);
            const image::ImageSubresource &mip = chained.m_subresources[1];

            // Border texel (1,0) blends one red column with one transparent
            // column: partial alpha, but full-bright red (no black drag).
            const std::array<unsigned int, 4> edge = texelOf_(mip, 1u, 0u, 4u);
            PPR_TEST_ASSERT(edge[3] >= 100u and edge[3] <= 160u);
            PPR_TEST_ASSERT(edge[0] >= 250u);
            PPR_TEST_ASSERT(edge[1] <= 4u);
            PPR_TEST_ASSERT(edge[2] <= 4u);

            // Fully covered texel stays opaque solid red.
            const std::array<unsigned int, 4> solid = texelOf_(mip, 0u, 0u, 4u);
            PPR_TEST_ASSERT(solid[3] == 255u);
            PPR_TEST_ASSERT(solid[0] >= 250u);
            PPR_TEST_ASSERT(solid[1] <= 4u);
            PPR_TEST_ASSERT(solid[2] <= 4u);

            // Fully transparent texels stay transparent: expansion recolors,
            // it never invents alpha.
            const std::array<unsigned int, 4> clear = texelOf_(mip, 3u, 3u, 4u);
            PPR_TEST_ASSERT(clear[3] == 0u);
        };
    } // namespace MipsBleed
} // namespace pP::tests::detail

namespace pP::tests::detail {
    namespace MipsQuality {
        [[nodiscard]] image::ImageAsset makeRamp_(const u32 size) {
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
                    const unsigned int ramp = (x + y) * 255u / (2u * (size - 1u));
                    (*view)[at + 0u] = static_cast<std::byte>(ramp);
                    (*view)[at + 1u] = static_cast<std::byte>(ramp);
                    (*view)[at + 2u] = static_cast<std::byte>(ramp);
                    (*view)[at + 3u] = std::byte{0xFF};
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
            built.m_dimension = image::EImageDimension::image2d;
            built.m_format = image::ENativeImageFormat::rgba8_linear;
            built.m_tag = image::EBlockTag::none;
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

        void pushU32_(std::vector<std::byte> &out, const u32 value) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFFu));
            }
        }

        // Minimal single-mip 4x4 DXT1 DDS fixture (same shape as the image
        // group): magic + 124-byte header + one 8-byte block.
        [[nodiscard]] mem::SharedBuffer makeDxt1Fixture_() {
            std::vector<std::byte> bytes{};
            pushU32_(bytes, 0x20534444u);
            pushU32_(bytes, 124u);
            pushU32_(bytes, 0x000A1007u);
            pushU32_(bytes, 4u);
            pushU32_(bytes, 4u);
            pushU32_(bytes, 8u);
            pushU32_(bytes, 0u);
            pushU32_(bytes, 1u);
            for (int i = 0; i < 11; ++i) {
                pushU32_(bytes, 0u);
            }
            pushU32_(bytes, 32u);
            pushU32_(bytes, 0x4u);
            pushU32_(bytes, 0x31545844u);
            for (int i = 0; i < 5; ++i) {
                pushU32_(bytes, 0u);
            }
            pushU32_(bytes, 0x1000u);
            for (int i = 0; i < 4; ++i) {
                pushU32_(bytes, 0u);
            }
            pushU32_(bytes, 0x001FF800u);
            pushU32_(bytes, 0x00000000u);
            return mem::SharedBuffer::clone(mem::SharedBufferView{bytes.data(), bytes.size()});
        }

        PPR_UNIT_TEST (mips_hq_within_bounds) {
            constexpr u32 kSize = 64u;
            image::ImageAsset stepped = makeRamp_(kSize);
            image::ImageAsset direct = makeRamp_(kSize);
            PPR_TEST_ASSERT(stepped.m_width == kSize);
            PPR_TEST_ASSERT(direct.m_width == kSize);

            image::MipGenDesc hq{};
            hq.m_is_high_quality = true;
            PPR_TEST_ASSERT(not image::generateMipChain(stepped, image::MipGenDesc{}));
            PPR_TEST_ASSERT(not image::generateMipChain(direct, hq));
            PPR_TEST_ASSERT(stepped.m_mip_count == direct.m_mip_count);
            PPR_TEST_ASSERT(stepped.m_mip_count == 5u);
            PPR_TEST_ASSERT(stepped.m_subresources.size() == direct.m_subresources.size());

            // Equivalence bound: smooth data keeps every level within a few
            // LSB whether filtered stepwise or straight from the top.
            unsigned int worst = 0u;
            for (u32 level = 0u; level < stepped.m_mip_count; ++level) {
                const mem::SharedBufferView a = stepped.m_subresources[level].m_view.getBufferData();
                const mem::SharedBufferView b = direct.m_subresources[level].m_view.getBufferData();
                PPR_TEST_ASSERT(a.size() == b.size());
                for (std::size_t i = 0u; i < a.size(); ++i) {
                    const unsigned int da = std::to_integer<unsigned int>(a[i]);
                    const unsigned int db = std::to_integer<unsigned int>(b[i]);
                    worst = std::max(worst, da > db ? da - db : db - da);
                }
            }
            PPR_TEST_ASSERT(worst <= 16u);

            // Blocked passthrough rejects generation: KTX2/DDS-embedded chains
            // stay untouched, never decoded to recompress.
            const mem::SharedBuffer dds = makeDxt1Fixture_();
            PPR_TEST_ASSERT(dds.isValid());
            Expected<image::ImageAsset> blocked =
                    image::decodeToBlocks(dds.getBufferData(), ".dds", image::EBlockTag::bc1, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(blocked.has_value());
            PPR_TEST_ASSERT(blocked->m_mip_count == 1u);
            const std::error_code rejected = image::generateMipChain(*blocked, image::MipGenDesc{});
            PPR_TEST_ASSERT(rejected == image::make_error_code(image::errc::function_not_supported));
            PPR_TEST_ASSERT(blocked->m_mip_count == 1u);
            PPR_TEST_ASSERT(blocked->m_subresources.size() == 1u);
        };
    } // namespace MipsQuality
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest image_mips = UnitTest::Named("mips") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::MipsChain::mips_chain_rules,
            detail::MipsCoverage::mips_coverage_preserved,
            detail::MipsBleed::mips_no_bleed_at_edges,
            detail::MipsQuality::mips_hq_within_bounds,
        });
    };

    const UnitTest &imageMipsTests() noexcept {
        return image_mips;
    }
} // namespace pP::tests
