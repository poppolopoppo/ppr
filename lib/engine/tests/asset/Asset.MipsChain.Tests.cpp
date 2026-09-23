module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.image;
import std;

// Chain rules: halving to 4 px (mipCount = FloorLog2(min) - 1),
// level 0 preserved verbatim, tight pitches, one frozen storage, chain bytes
// against the decode cap. One focused test per TU.
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
            PPR_TEST_ASSERT(chained.m_format == image::NativeImageFormat::rgba8_srgb);
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

namespace pP::tests {
    const UnitTest mips_chain = UnitTest::Named("mips_chain") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::MipsChain::mips_chain_rules,
        });
    };

    const UnitTest &mipsChainTests() noexcept {
        return mips_chain;
    }
} // namespace pP::tests
