module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/image/image.hpp>

module engine.tests.asset;

import engine.core;
import engine.image;
import std;

namespace pP::tests::detail {
    namespace Image {
        [[nodiscard]] mango::image::Format testRgbaFormat() {
            using mango::image::Format;
            return Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8);
        }

        // Temp fixtures stage into the test working directory at runtime (no
        // binaries are committed); decode stages then exercise the
        // file -> mapFile -> decode lifecycle from §2.5.
        [[nodiscard]] std::filesystem::path fixtureDir() {
            const std::filesystem::path dir = std::filesystem::current_path() / "temp_image_fixtures";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            return dir;
        }

        [[nodiscard]] mem::SharedBuffer encodeSurfaceFixture(
            const std::string &name, const u32 width, const u32 height, const std::span<const std::byte> rgba) {
            const std::filesystem::path path = fixtureDir() / name;
            const mango::image::Surface surface{
                static_cast<int>(width),
                static_cast<int>(height),
                testRgbaFormat(),
                static_cast<std::size_t>(width) * 4u,
                rgba.data(),
            };
            const mango::image::ImageEncodeStatus status = surface.save(path.string());
            if (not static_cast<bool>(status)) {
                return {};
            }
            if (Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(path); mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        void pushU32_(std::vector<std::byte> &out, const u32 value) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFFu));
            }
        }

        // Minimal single-mip 4x4 DXT1 DDS: magic + 124-byte header + one 8-byte
        // block (solid red: color0 RGB565 0xF800, all indices select color0).
        [[nodiscard]] mem::SharedBuffer makeDxt1Fixture() {
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
            // color0 RGB565 0xF800 (red), color1 0x001F, indices select color0.
            pushU32_(bytes, 0x001FF800u);
            pushU32_(bytes, 0x00000000u);
            return mem::SharedBuffer::clone(mem::SharedBufferView{bytes.data(), bytes.size()});
        }

        void pushU64_(std::vector<std::byte> &out, const u64 value) {
            for (int i = 0; i < 8; ++i) {
                out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFFu));
            }
        }

        // Minimal KTX2 container: 12-byte magic, 9 header fields, zero
        // DFD/KVD/SGD index, one level entry, then the raw payload. vkFormat 131
        // is VK_FORMAT_BC1_RGB_UNORM_BLOCK with supercompression NONE, so Mango
        // serves level bytes straight through memory(); vkFormat 37 is
        // R8G8B8A8_UNORM (uncompressed, never a valid block source).
        [[nodiscard]] mem::SharedBuffer makeKtx2Fixture(const u32 width, const u32 height, const u32 vk_format,
                                                        const std::vector<std::byte> &payload, const u32 supercompression = 0u,
                                                        const std::size_t uncompressed_size = 0u) {
            std::vector<std::byte> bytes{};
            for (const u32 magic: {
                     0xABu, 0x4Bu, 0x54u, 0x58u, 0x20u, 0x32u, 0x30u, 0xBBu,
                     0x0Du, 0x0Au, 0x1Au, 0x0Au
                 }) {
                bytes.push_back(static_cast<std::byte>(magic));
            }
            pushU32_(bytes, vk_format);
            pushU32_(bytes, 1u);
            pushU32_(bytes, width);
            pushU32_(bytes, height);
            pushU32_(bytes, 0u);
            pushU32_(bytes, 0u);
            pushU32_(bytes, 1u);
            pushU32_(bytes, 1u);
            pushU32_(bytes, supercompression);
            pushU32_(bytes, 0u);
            pushU32_(bytes, 0u);
            pushU32_(bytes, 0u);
            pushU32_(bytes, 0u);
            pushU64_(bytes, 0u);
            pushU64_(bytes, 0u);
            pushU64_(bytes, 104u);
            pushU64_(bytes, static_cast<u64>(payload.size()));
            const u64 raw_size =
                    uncompressed_size == 0u ? static_cast<u64>(payload.size()) : static_cast<u64>(uncompressed_size);
            pushU64_(bytes, raw_size);
            bytes.insert(bytes.end(), payload.begin(), payload.end());
            return mem::SharedBuffer::clone(mem::SharedBufferView{bytes.data(), bytes.size()});
        }

        [[nodiscard]] u32 adler32_(const std::span<const std::byte> data) noexcept {
            u32 a = 1u;
            u32 b = 0u;
            for (const std::byte value: data) {
                a = (a + static_cast<u32>(static_cast<unsigned char>(value))) % 65521u;
                b = (b + a) % 65521u;
            }
            return (b << 16u) | a;
        }

        // Minimal zlib stream with stored (uncompressed) deflate blocks: enough
        // for Mango's KTX2 ZLIB supercompression path, no external compressor.
        [[nodiscard]] std::vector<std::byte> zlibStored_(const std::span<const std::byte> data) {
            std::vector<std::byte> out{};
            out.push_back(static_cast<std::byte>(0x78));
            out.push_back(static_cast<std::byte>(0x01));
            std::size_t off = 0u;
            while (off < data.size()) {
                const std::size_t chunk = std::min<std::size_t>(65535u, data.size() - off);
                const bool last = off + chunk == data.size();
                out.push_back(static_cast<std::byte>(last ? 0x01 : 0x00));
                const u32 len = static_cast<u32>(chunk);
                out.push_back(static_cast<std::byte>(len & 0xFFu));
                out.push_back(static_cast<std::byte>((len >> 8u) & 0xFFu));
                out.push_back(static_cast<std::byte>(~len & 0xFFu));
                out.push_back(static_cast<std::byte>((~len >> 8u) & 0xFFu));
                out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(off),
                    data.begin() + static_cast<std::ptrdiff_t>(off + chunk));
                off += chunk;
            }
            const u32 adler = adler32_(data);
            for (int i = 3; i >= 0; --i) {
                out.push_back(static_cast<std::byte>((adler >> (i * 8)) & 0xFFu));
            }
            return out;
        }

        [[nodiscard]] bool bytesEqual_(const mem::SharedBufferView a, const std::span<const std::byte> b) noexcept {
            return a.size() == b.size() and std::ranges::equal(a, b);
        }

        PPR_UNIT_TEST (rgba_png_color_is_srgb) {
            constexpr u32 kWidth = 4u;
            constexpr u32 kHeight = 2u;
            constexpr std::array<std::byte, kWidth * kHeight * 4u> kPixels{
                std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0x00}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0x00}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF},
                std::byte{0xFF}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF},
                std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0xFF},
                std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0xFF},
            };
            const mem::SharedBuffer file = encodeSurfaceFixture("rgba_color.png", kWidth, kHeight, kPixels);
            PPR_TEST_ASSERT(file.isValid());

            const Expected<image::ImageAsset> decoded =
                    image::decodeToRgba8(file.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(decoded.has_value());
            const image::ImageAsset &decoded_asset = *decoded;
            PPR_TEST_ASSERT(decoded_asset.m_width == kWidth);
            PPR_TEST_ASSERT(decoded_asset.m_height == kHeight);
            PPR_TEST_ASSERT(decoded_asset.m_mip_count == 1u);
            PPR_TEST_ASSERT(decoded_asset.m_dimension == image::EImageDimension::image2d);
            PPR_TEST_ASSERT(decoded_asset.m_format == image::ENativeImageFormat::rgba8_srgb);
            PPR_TEST_ASSERT(decoded_asset.m_is_srgb);
            PPR_TEST_ASSERT(not decoded_asset.m_is_block);
            PPR_TEST_ASSERT(decoded_asset.m_tag == image::EBlockTag::none);
            PPR_TEST_ASSERT(decoded_asset.m_storage.isMaterialized());
            PPR_TEST_ASSERT(decoded_asset.m_storage.getBufferData().size() == kPixels.size());
            PPR_TEST_ASSERT(decoded_asset.m_subresources.size() == 1u);
            const image::ImageSubresource &sub = decoded_asset.m_subresources.front();
            PPR_TEST_ASSERT(sub.m_row_pitch == static_cast<u64>(kWidth) * 4u);
            PPR_TEST_ASSERT(sub.m_slice_pitch == static_cast<u64>(kPixels.size()));
            PPR_TEST_ASSERT(sub.m_row_pitch > 0u);
            PPR_TEST_ASSERT(bytesEqual_(sub.m_view.getBufferData(), kPixels));
        };

        PPR_UNIT_TEST (rgba_data_usage_is_linear) {
            constexpr std::array<std::byte, 8u> kPixels{
                std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0xFF},
                std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0xFF},
            };
            const mem::SharedBuffer file = encodeSurfaceFixture("rgba_data.png", 2u, 1u, kPixels);
            PPR_TEST_ASSERT(file.isValid());

            const Expected<image::ImageAsset> decoded =
                    image::decodeToRgba8(file.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::data);
            PPR_TEST_ASSERT(decoded.has_value());
            PPR_TEST_ASSERT(decoded->m_format == image::ENativeImageFormat::rgba8_linear);
            PPR_TEST_ASSERT(not decoded->m_is_srgb);
        };

        PPR_UNIT_TEST (rgba_jpg_decodes) {
            constexpr std::array<std::byte, 48u> kPixels{};
            const mem::SharedBuffer file = encodeSurfaceFixture("rgba_lossy.jpg", 4u, 3u, kPixels);
            PPR_TEST_ASSERT(file.isValid());

            const Expected<image::ImageAsset> decoded =
                    image::decodeToRgba8(file.getBufferData(), ".jpg", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(decoded.has_value());
            PPR_TEST_ASSERT(decoded->m_width == 4u);
            PPR_TEST_ASSERT(decoded->m_height == 3u);
            PPR_TEST_ASSERT(decoded->m_format == image::ENativeImageFormat::rgba8_srgb);
        };

        PPR_UNIT_TEST (rgba_rejects_unsupported_extension) {
            constexpr std::array<std::byte, 8u> kPixels{
                std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xFF},
                std::byte{0xDD}, std::byte{0xEE}, std::byte{0xFF}, std::byte{0xFF},
            };
            const mem::SharedBuffer file = encodeSurfaceFixture("rgba_scope.png", 2u, 1u, kPixels);
            PPR_TEST_ASSERT(file.isValid());

            // BMP decodes in Mango but is outside the MVP scope: still invalid_argument.
            const Expected<image::ImageAsset> decoded =
                    image::decodeToRgba8(file.getBufferData(), ".bmp", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(not decoded.has_value());
            PPR_TEST_ASSERT(decoded.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (rgba_rejects_empty_and_corrupt) {
            constexpr std::array<std::byte, 16u> kGarbage{
                std::byte{'P'}, std::byte{'P'}, std::byte{'R'}, std::byte{'!'},
                std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
                std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B},
            };
            const Expected<image::ImageAsset> empty = image::decodeToRgba8(
                mem::SharedBufferView{}, ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(not empty.has_value());
            PPR_TEST_ASSERT(empty.error() == std::errc::invalid_argument);

            const Expected<image::ImageAsset> corrupt = image::decodeToRgba8(
                mem::SharedBufferView{kGarbage.data(), kGarbage.size()},
                ".png",
                image::ImageDecodeDesc{},
                image::EImageUsage::color);
            PPR_TEST_ASSERT(not corrupt.has_value());
            PPR_TEST_ASSERT(corrupt.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (rgba_flip_v_reverses_rows) {
            constexpr std::array<std::byte, 16u> kPixels{
                std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0x00}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0x00}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF},
                std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
            };
            const mem::SharedBuffer file = encodeSurfaceFixture("rgba_flip.png", 2u, 2u, kPixels);
            PPR_TEST_ASSERT(file.isValid());

            image::ImageDecodeDesc flipped{};
            flipped.m_use_flip_v = true;
            const Expected<image::ImageAsset> straight = image::decodeToRgba8(
                file.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
            const Expected<image::ImageAsset> flipped_asset =
                    image::decodeToRgba8(file.getBufferData(), ".png", flipped, image::EImageUsage::color);
            PPR_TEST_ASSERT(straight.has_value());
            PPR_TEST_ASSERT(flipped_asset.has_value());

            const mem::SharedBufferView top = straight->m_subresources.front().m_view.getBufferData();
            const mem::SharedBufferView flipped_view = flipped_asset->m_subresources.front().m_view.getBufferData();
            PPR_TEST_ASSERT(top.size() == flipped_view.size());
            constexpr std::size_t kRow = 8u;
            PPR_TEST_ASSERT(bytesEqual_(flipped_view.subspan(0u, kRow), top.subspan(kRow, kRow)));
            PPR_TEST_ASSERT(bytesEqual_(flipped_view.subspan(kRow, kRow), top.subspan(0u, kRow)));
            PPR_TEST_ASSERT(flipped_asset->m_subresources.front().m_row_pitch == 8u);
        };

        PPR_UNIT_TEST (rgba_dds_decompresses) {
            const mem::SharedBuffer file = makeDxt1Fixture();
            PPR_TEST_ASSERT(file.isValid());

            const Expected<image::ImageAsset> decoded =
                    image::decodeToRgba8(file.getBufferData(), ".dds", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(decoded.has_value());
            PPR_TEST_ASSERT(decoded->m_width == 4u);
            PPR_TEST_ASSERT(decoded->m_height == 4u);
            PPR_TEST_ASSERT(not decoded->m_is_block);

            // Solid-red BC1 block: red dominates, alpha is opaque.
            const mem::SharedBufferView pixels = decoded->m_subresources.front().m_view.getBufferData();
            PPR_TEST_ASSERT(pixels.size() == 64u);
            PPR_TEST_ASSERT(static_cast<unsigned char>(pixels[0]) > 200u);
            PPR_TEST_ASSERT(static_cast<unsigned char>(pixels[1]) < 50u);
            PPR_TEST_ASSERT(static_cast<unsigned char>(pixels[2]) < 50u);
            PPR_TEST_ASSERT(static_cast<unsigned char>(pixels[3]) == 255u);
        };

        PPR_UNIT_TEST (blocks_png_never_recompresses) {
            constexpr std::array<std::byte, 8u> kPixels{
                std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0xFF},
                std::byte{0x40}, std::byte{0x50}, std::byte{0x60}, std::byte{0xFF},
            };
            const mem::SharedBuffer png = encodeSurfaceFixture("blocks_no_recompress.png", 2u, 1u, kPixels);
            const mem::SharedBuffer jpg = encodeSurfaceFixture("blocks_no_recompress.jpg", 2u, 1u, kPixels);
            PPR_TEST_ASSERT(png.isValid());
            PPR_TEST_ASSERT(jpg.isValid());

            const Expected<image::ImageAsset> from_png =
                    image::decodeToBlocks(png.getBufferData(), ".png", image::EBlockTag::bc7, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(not from_png.has_value());
            PPR_TEST_ASSERT(from_png.error() == std::errc::function_not_supported);

            const Expected<image::ImageAsset> from_jpg =
                    image::decodeToBlocks(jpg.getBufferData(), ".jpg", image::EBlockTag::bc1, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(not from_jpg.has_value());
            PPR_TEST_ASSERT(from_jpg.error() == std::errc::function_not_supported);
        };

        PPR_UNIT_TEST (blocks_none_target_is_invalid) {
            const mem::SharedBuffer file = makeDxt1Fixture();
            PPR_TEST_ASSERT(file.isValid());

            const Expected<image::ImageAsset> decoded =
                    image::decodeToBlocks(file.getBufferData(), ".dds", image::EBlockTag::none, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(not decoded.has_value());
            PPR_TEST_ASSERT(decoded.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (blocks_dxt1_passthrough) {
            const mem::SharedBuffer file = makeDxt1Fixture();
            PPR_TEST_ASSERT(file.isValid());

            const Expected<image::ImageAsset> decoded =
                    image::decodeToBlocks(file.getBufferData(), ".dds", image::EBlockTag::bc1, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(decoded.has_value());
            const image::ImageAsset &block_asset = *decoded;
            PPR_TEST_ASSERT(block_asset.m_width == 4u);
            PPR_TEST_ASSERT(block_asset.m_height == 4u);
            PPR_TEST_ASSERT(block_asset.m_is_block);
            PPR_TEST_ASSERT(block_asset.m_tag == image::EBlockTag::bc1);
            PPR_TEST_ASSERT(block_asset.m_format == image::ENativeImageFormat::bc1_linear);
            PPR_TEST_ASSERT(block_asset.m_block_w == 4u);
            PPR_TEST_ASSERT(block_asset.m_block_h == 4u);
            PPR_TEST_ASSERT(block_asset.m_bytes_per_block == 16u / 2u);
            PPR_TEST_ASSERT(block_asset.m_storage.isMaterialized());
            PPR_TEST_ASSERT(block_asset.m_storage.getBufferData().size() == 8u);
            PPR_TEST_ASSERT(block_asset.m_subresources.size() == 1u);
            PPR_TEST_ASSERT(block_asset.m_subresources.front().m_row_pitch == 8u);
            PPR_TEST_ASSERT(block_asset.m_subresources.front().m_slice_pitch == 8u);
        };

        PPR_UNIT_TEST (blocks_mismatched_target_fails) {
            const mem::SharedBuffer file = makeDxt1Fixture();
            PPR_TEST_ASSERT(file.isValid());

            // BC1 source cannot serve a BC7 transcode target.
            const Expected<image::ImageAsset> decoded =
                    image::decodeToBlocks(file.getBufferData(), ".dds", image::EBlockTag::bc7, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(not decoded.has_value());
            PPR_TEST_ASSERT(decoded.error() == std::errc::function_not_supported);
        };

        PPR_UNIT_TEST (blocks_ktx2_concurrent_transcode_matches_serial) {
            // 256x256 BC1 is exactly one 32 KiB SmallPage job buffer; the ZLIB
            // twin keeps the same inflated bytes with real inflate work inside
            // memory() — the region the old global mutex serialized.
            constexpr u32 kWidth = 256u;
            constexpr u32 kHeight = 256u;
            std::vector<std::byte> payload{};
            payload.reserve(static_cast<std::size_t>(kWidth / 4u) * (kHeight / 4u) * 8u);
            for (std::size_t i = 0u; i < payload.capacity(); ++i) {
                payload.push_back(static_cast<std::byte>((i * 31u + (i >> 8u)) & 0xFFu));
            }
            const std::span<const std::byte> payload_view{payload.data(), payload.size()};
            const mem::SharedBuffer file = makeKtx2Fixture(kWidth, kHeight, 131u, payload);
            PPR_TEST_ASSERT(file.isValid());
            const std::vector<std::byte> deflated =
                    zlibStored_(std::span<const std::byte>{payload.data(), payload.size()});
            const mem::SharedBuffer packed =
                    makeKtx2Fixture(kWidth, kHeight, 131u, deflated, 3u, payload.size());
            PPR_TEST_ASSERT(packed.isValid());

            auto probe = [&](const mem::SharedBuffer &source, const char *label, const int iters) {
                const Expected<image::ImageAsset> reference = image::decodeToBlocks(source.getBufferData(),
                    ".ktx2", image::EBlockTag::bc1, image::ImageDecodeDesc{});
                PPR_TEST_ASSERT(reference.has_value());
                PPR_TEST_ASSERT(reference->m_width == kWidth);
                PPR_TEST_ASSERT(reference->m_height == kHeight);
                PPR_TEST_ASSERT(reference->m_is_block);
                PPR_TEST_ASSERT(reference->m_tag == image::EBlockTag::bc1);
                PPR_TEST_ASSERT(reference->m_format == image::ENativeImageFormat::bc1_linear);
                PPR_TEST_ASSERT(reference->m_storage.getBufferData().size() == payload.size());
                PPR_TEST_ASSERT(bytesEqual_(reference->m_subresources.front().m_view.getBufferData(), payload_view));
                const hash_t expected =
                        image::contentHash(reference->m_subresources.front().m_view.getBufferData());

                // Throughput probe prints even when a later rejection regresses;
                // byte-identity is asserted (no timing assert, machines vary).
                constexpr int kThreads = 8;
                std::atomic<int> failures{0};
                std::atomic<u64> decoded_bytes{0};
                const auto start = std::chrono::steady_clock::now();
                std::vector<std::thread> workers{};
                for (int t = 0; t < kThreads; ++t) {
                    workers.emplace_back([&] {
                        for (int i = 0; i < iters; ++i) {
                            const Expected<image::ImageAsset> decoded = image::decodeToBlocks(
                                source.getBufferData(), ".ktx2", image::EBlockTag::bc1, image::ImageDecodeDesc{});
                            if (not decoded.has_value() or
                                image::contentHash(decoded->m_subresources.front().m_view.getBufferData()) !=
                                expected) {
                                failures.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }
                            decoded_bytes.fetch_add(static_cast<u64>(decoded->m_storage.getBufferData().size()),
                                std::memory_order_relaxed);
                        }
                    });
                }
                for (std::thread &worker: workers) {
                    worker.join();
                }
                const auto elapsed = std::chrono::steady_clock::now() - start;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                const double mb_per_s = ms > 0 ? static_cast<double>(decoded_bytes.load()) / 1048576.0 /
                        (static_cast<double>(ms) / 1000.0) : 0.0;
                std::printf("[ktx2-throughput-%s] threads=%d iters=%d bytes=%llu ms=%lld (%.1f MB/s)\n", label,
                    kThreads, iters, static_cast<unsigned long long>(decoded_bytes.load()),
                    static_cast<long long>(ms), mb_per_s);
                PPR_TEST_ASSERT(failures.load(std::memory_order_relaxed) == 0);
            };
            probe(file, "plain", 128);
            probe(packed, "zlib", 128);

            // Mismatched targets and uncompressed sources fail closed, never reinterpreted.
            const Expected<image::ImageAsset> mismatched = image::decodeToBlocks(
                file.getBufferData(), ".ktx2", image::EBlockTag::bc7, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(not mismatched.has_value());
            PPR_TEST_ASSERT(mismatched.error() == std::errc::function_not_supported);

            const std::vector<std::byte> rgba(4u * 4u * 4u, std::byte{0x7F});
            const mem::SharedBuffer plain = makeKtx2Fixture(4u, 4u, 37u, rgba);
            PPR_TEST_ASSERT(plain.isValid());
            const Expected<image::ImageAsset> reinterpreted = image::decodeToBlocks(
                plain.getBufferData(), ".ktx2", image::EBlockTag::bc1, image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(not reinterpreted.has_value());
            PPR_TEST_ASSERT(reinterpreted.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (block_geometry_math) {
            PPR_TEST_ASSERT(image::rowPitchFor(4u, image::EBlockTag::bc1) == 8u);
            PPR_TEST_ASSERT(image::slicePitchFor(4u, 4u, image::EBlockTag::bc1) == 8u);
            PPR_TEST_ASSERT(image::rowPitchFor(5u, image::EBlockTag::bc1) == 16u);
            PPR_TEST_ASSERT(image::slicePitchFor(5u, 5u, image::EBlockTag::bc3) == 64u);
            PPR_TEST_ASSERT(image::rowPitchFor(3u, image::EBlockTag::none) == 12u);
            PPR_TEST_ASSERT(image::slicePitchFor(3u, 2u, image::EBlockTag::none) == 24u);
            PPR_TEST_ASSERT(image::blockWidthOf(image::EBlockTag::astc6x6) == 6u);
            PPR_TEST_ASSERT(image::blockHeightOf(image::EBlockTag::astc8x8) == 8u);
            PPR_TEST_ASSERT(image::bytesPerBlockOf(image::EBlockTag::bc4) == 8u);
            PPR_TEST_ASSERT(image::bytesPerBlockOf(image::EBlockTag::bc5) == 16u);
            PPR_TEST_ASSERT(image::bytesPerBlockOf(image::EBlockTag::astc4x4) == 16u);
            PPR_TEST_ASSERT(image::rowPitchFor(8u, image::EBlockTag::astc8x8) == 16u);
            PPR_TEST_ASSERT(image::slicePitchFor(7u, 7u, image::EBlockTag::astc6x6) == 64u);
        };

        PPR_UNIT_TEST (format_predicate_roundtrip) {
            constexpr image::ENativeImageFormat kFormats[]{
                image::ENativeImageFormat::rgba8_linear,
                image::ENativeImageFormat::rgba8_srgb,
                image::ENativeImageFormat::bc1_linear,
                image::ENativeImageFormat::bc1_srgb,
                image::ENativeImageFormat::bc3_linear,
                image::ENativeImageFormat::bc3_srgb,
                image::ENativeImageFormat::bc4_linear,
                image::ENativeImageFormat::bc5_linear,
                image::ENativeImageFormat::bc7_linear,
                image::ENativeImageFormat::bc7_srgb,
                image::ENativeImageFormat::astc4x4_linear,
                image::ENativeImageFormat::astc4x4_srgb,
                image::ENativeImageFormat::astc6x6_linear,
                image::ENativeImageFormat::astc6x6_srgb,
                image::ENativeImageFormat::astc8x8_linear,
                image::ENativeImageFormat::astc8x8_srgb,
            };

            for (const image::ENativeImageFormat format: kFormats) {
                const bool blocked = image::isBlocked(format);
                PPR_TEST_ASSERT(image::isSrgb(format) or not image::isSrgb(format));
                if (not blocked) {
                    PPR_TEST_ASSERT(image::blockTagOf(format) == image::EBlockTag::none);
                } else {
                    const image::EBlockTag tag = image::blockTagOf(format);
                    PPR_TEST_ASSERT(tag != image::EBlockTag::none);
                    PPR_TEST_ASSERT(image::blockWidthOf(tag) >= 4u);
                    PPR_TEST_ASSERT(image::blockHeightOf(tag) >= 4u);
                    PPR_TEST_ASSERT(image::bytesPerBlockOf(tag) >= 8u);
                }
            }

            PPR_TEST_ASSERT(image::isSrgb(image::ENativeImageFormat::rgba8_srgb));
            PPR_TEST_ASSERT(not image::isSrgb(image::ENativeImageFormat::rgba8_linear));
            PPR_TEST_ASSERT(not image::isBlocked(image::ENativeImageFormat::rgba8_linear));
            PPR_TEST_ASSERT(image::isBlocked(image::ENativeImageFormat::bc7_srgb));
        };

        PPR_UNIT_TEST (content_hash_is_content_keyed) {
            constexpr std::array<std::byte, 8u> kPixels{
                std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0xFF},
                std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0xFF},
            };
            const mem::SharedBuffer first = encodeSurfaceFixture("hash_first.png", 2u, 1u, kPixels);
            const mem::SharedBuffer second = encodeSurfaceFixture("hash_second.png", 2u, 1u, kPixels);
            PPR_TEST_ASSERT(first.isValid());
            PPR_TEST_ASSERT(second.isValid());

            const Expected<image::ImageAsset> a =
                    image::decodeToRgba8(first.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
            const Expected<image::ImageAsset> b = image::decodeToRgba8(
                second.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(a.has_value());
            PPR_TEST_ASSERT(b.has_value());

            // Distinct owners, identical content: owner identity never decides.
            PPR_TEST_ASSERT(a->m_storage != b->m_storage);
            const hash_t ha = image::contentHash(a->m_subresources.front().m_view.getBufferData());
            const hash_t hb = image::contentHash(b->m_subresources.front().m_view.getBufferData());
            PPR_TEST_ASSERT(ha == hb);
            PPR_TEST_ASSERT(ha == hash::contiguousRange(std::span<const std::byte>{kPixels.data(), kPixels.size()}));
        };

        PPR_UNIT_TEST (frozen_asset_shares_across_threads) {
            constexpr std::array<std::byte, 16u> kPixels{
                std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
                std::byte{0xCA}, std::byte{0xFE}, std::byte{0xBA}, std::byte{0xBE},
                std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
            };
            const mem::SharedBuffer file = encodeSurfaceFixture("frozen_share.png", 2u, 2u, kPixels);
            PPR_TEST_ASSERT(file.isValid());
            const Expected<image::ImageAsset> decoded =
                    image::decodeToRgba8(file.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(decoded.has_value());

            // Frozen SharedBuffers share by value; decoders + UniqueBuffers stay per-job.
            const image::ImageAsset shared = *decoded;
            const hash_t expected = image::contentHash(shared.m_subresources.front().m_view.getBufferData());
            std::atomic<int> mismatches{0};
            std::vector<std::thread> workers{};
            for (int i = 0; i < 4; ++i) {
                workers.emplace_back([&] {
                    const image::ImageAsset local = shared;
                    const mem::SharedBufferView view = local.m_subresources.front().m_view.getBufferData();
                    if (view.size() != kPixels.size() or image::contentHash(view) != expected or not bytesEqual_(view, kPixels)) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (std::thread &worker: workers) {
                worker.join();
            }
            PPR_TEST_ASSERT(mismatches.load(std::memory_order_relaxed) == 0);
        };
    } // namespace Image
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest &imageMipsTests() noexcept;

    const UnitTest image = UnitTest::Named("image") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Image::rgba_png_color_is_srgb,
            detail::Image::rgba_data_usage_is_linear,
            detail::Image::rgba_jpg_decodes,
            detail::Image::rgba_rejects_unsupported_extension,
            detail::Image::rgba_rejects_empty_and_corrupt,
            detail::Image::rgba_flip_v_reverses_rows,
            detail::Image::rgba_dds_decompresses,
            detail::Image::blocks_png_never_recompresses,
            detail::Image::blocks_none_target_is_invalid,
            detail::Image::blocks_dxt1_passthrough,
            detail::Image::blocks_mismatched_target_fails,
            detail::Image::blocks_ktx2_concurrent_transcode_matches_serial,
            detail::Image::block_geometry_math,
            detail::Image::format_predicate_roundtrip,
            detail::Image::content_hash_is_content_keyed,
            detail::Image::frozen_asset_shares_across_threads,
            imageMipsTests(),
        });
    };

    const UnitTest &imageTests() noexcept {
        return image;
    }
} // namespace pP::tests
