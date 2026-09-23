module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.image;
import std;

// Quality gate: HQ-from-top stays within equivalence bounds of the
// incremental default, and blocked (KTX2/DDS passthrough) assets reject
// generation so embedded chains are never decoded to recompress.
// One focused test per TU.
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
            hq.m_high_quality = true;
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
                    image::decodeToBlocks(dds.getBufferData(), ".dds", image::BlockTag::bc1, image::ImageDecodeDesc{});
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
    const UnitTest mips_quality = UnitTest::Named("mips_quality") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::MipsQuality::mips_hq_within_bounds,
        });
    };

    const UnitTest &mipsQualityTests() noexcept {
        return mips_quality;
    }
} // namespace pP::tests
