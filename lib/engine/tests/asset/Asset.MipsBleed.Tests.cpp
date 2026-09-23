module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.image;
import std;

// Anti-bleed: transparent texels carry expanded neighbor color
// (chamfer distance-field) instead of clear-black, so translucent edges keep
// full-bright color and expansion never invents alpha. One focused test per TU.
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
            built.m_dimension = image::ImageDimension::image2d;
            built.m_format = image::NativeImageFormat::rgba8_srgb;
            built.m_tag = image::BlockTag::none;
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

namespace pP::tests {
    const UnitTest mips_bleed = UnitTest::Named("mips_bleed") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::MipsBleed::mips_no_bleed_at_edges,
        });
    };

    const UnitTest &mipsBleedTests() noexcept {
        return mips_bleed;
    }
} // namespace pP::tests
