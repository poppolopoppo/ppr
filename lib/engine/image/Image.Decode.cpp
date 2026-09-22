module;
#include "pP/Macros.h"

#include <mango/image/image.hpp>

module engine.image;

import :types;
import :decode;
import engine.core;
import engine.math;

import std;

namespace pP::image {
    namespace {
        // Extensions are normalized to Mango's lowercase dot-form (".png"); the
        // decoder is selected by this hint, sRGB never comes from the filename.
        [[nodiscard]] std::string normalizeExtension_(const std::string_view ext) {
            std::string dotted{ext};
            std::ranges::transform(dotted, dotted.begin(), [](const char c) noexcept {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            if (dotted.empty() or dotted.front() != '.') {
                dotted.insert(dotted.begin(), '.');
            }
            return dotted;
        }

        [[nodiscard]] bool isRgbaSource_(const std::string_view dotted) noexcept {
            return dotted == ".png" or dotted == ".jpg" or dotted == ".jpeg" or dotted == ".ktx2" or dotted == ".dds";
        }

        [[nodiscard]] bool isCompressedSource_(const std::string_view dotted) noexcept {
            return dotted == ".ktx2" or dotted == ".dds";
        }

        // Parser-safety floors (Phase 5 hardening): Mango's image parsers do
        // sequential unchecked reads, so a short input over-reads past the end
        // (ASan container-overflow in ParserPNG::read_IHDR on truncation).
        // Reject below-floor inputs deterministically before the decoder runs.
        [[nodiscard]] std::size_t minInputBytesFor_(const std::string_view dotted) noexcept {
            if (dotted == ".png") {
                return 33u; // signature(8) + len(4) + "IHDR"(4) + data(13) + crc(4)
            }
            if (dotted == ".ktx2") {
                return 80u; // 12-byte magic + fixed header fields
            }
            if (dotted == ".dds") {
                return 128u; // magic(4) + DDS_HEADER(124)
            }
            return 16u; // JPG: SOI marker plus minimal segment presence
        }

        // PNG chunk-chain pre-validation: walks len/type/data/crc links with
        // overflow-safe bounds, requiring IHDR first (len 13, Mango's own rule)
        // and a terminating IEND. Closes length-jump over-reads (a flipped
        // high length byte would otherwise skip past EOF) and truncations.
        // Lenient on chunk types and post-IEND bytes: anything Mango decodes
        // today still validates; only structurally unsound chains reject.
        [[nodiscard]] bool validatePngChunks_(const std::span<const std::byte> bytes) noexcept {
            const std::size_t size = bytes.size();
            if (size < 33u) {
                return false;
            }
            static constexpr std::byte kSignature[8] = {
                std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
                std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
            };
            if (not std::ranges::equal(std::span{bytes.data(), 8u}, std::span{kSignature, 8u})) {
                return false;
            }
            auto readBe32 = [&](const std::size_t off) noexcept {
                u32 value = 0u;
                std::memcpy(&value, bytes.data() + off, sizeof(value));
                return (value >> 24u) | ((value >> 8u) & 0xFF00u) | ((value << 8u) & 0xFF0000u) |
                       (value << 24u);
            };
            auto typeIs = [&](const std::size_t off, const std::string_view want) noexcept {
                for (std::size_t k = 0u; k < 4u; ++k) {
                    if (bytes[off + 4u + k] != static_cast<std::byte>(want[k])) {
                        return false;
                    }
                }
                return true;
            };
            std::size_t off = 8u;
            bool seen_ihdr = false;
            while (true) {
                if (off + 8u > size) {
                    return false;
                }
                const u64 len = readBe32(off);
                // Overflow-safe data+crc fit: len + 12 <= size - off.
                if (len + 12u > static_cast<u64>(size) - static_cast<u64>(off)) {
                    return false;
                }
                if (not seen_ihdr) {
                    if (len != 13u or not typeIs(off, "IHDR")) {
                        return false;
                    }
                    seen_ihdr = true;
                } else if (typeIs(off, "IEND")) {
                    return true;
                }
                off += static_cast<std::size_t>(len) + 12u;
            }
        }

        [[nodiscard]] mango::image::Format rgba8Format_() {
            using mango::image::Format;
            return Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8);
        }

        [[nodiscard]] u32 mangoCompressionFor_(const BlockTag want, const bool is_srgb) noexcept {
            using mango::image::TextureCompression;
            switch (want) {
                case BlockTag::bc1: return is_srgb ? TextureCompression::DXT1_SRGB : TextureCompression::DXT1;
                case BlockTag::bc3: return is_srgb ? TextureCompression::DXT5_SRGB : TextureCompression::DXT5;
                case BlockTag::bc4: return TextureCompression::RGTC1_RED;
                case BlockTag::bc5: return TextureCompression::RGTC2_RG;
                case BlockTag::bc7:
                    return is_srgb ? TextureCompression::BPTC_SRGB_ALPHA_UNORM : TextureCompression::BPTC_RGBA_UNORM;
                case BlockTag::astc4x4: return is_srgb ? TextureCompression::ASTC_SRGB_4x4 : TextureCompression::ASTC_UNORM_4x4;
                case BlockTag::astc6x6: return is_srgb ? TextureCompression::ASTC_SRGB_6x6 : TextureCompression::ASTC_UNORM_6x6;
                case BlockTag::astc8x8: return is_srgb ? TextureCompression::ASTC_SRGB_8x8 : TextureCompression::ASTC_UNORM_8x8;
                default: return TextureCompression::NONE;
            }
        }

        [[nodiscard]] NativeImageFormat nativeFormatFor_(const BlockTag want, const bool is_srgb) noexcept {
            switch (want) {
                case BlockTag::bc1: return is_srgb ? NativeImageFormat::bc1_srgb : NativeImageFormat::bc1_linear;
                case BlockTag::bc3: return is_srgb ? NativeImageFormat::bc3_srgb : NativeImageFormat::bc3_linear;
                case BlockTag::bc4: return NativeImageFormat::bc4_linear;
                case BlockTag::bc5: return NativeImageFormat::bc5_linear;
                case BlockTag::bc7: return is_srgb ? NativeImageFormat::bc7_srgb : NativeImageFormat::bc7_linear;
                case BlockTag::astc4x4:
                    return is_srgb ? NativeImageFormat::astc4x4_srgb : NativeImageFormat::astc4x4_linear;
                case BlockTag::astc6x6:
                    return is_srgb ? NativeImageFormat::astc6x6_srgb : NativeImageFormat::astc6x6_linear;
                case BlockTag::astc8x8:
                    return is_srgb ? NativeImageFormat::astc8x8_srgb : NativeImageFormat::astc8x8_linear;
                default: return NativeImageFormat::rgba8_linear;
            }
        }

        // Per-job scratch decode target: allocate -> materialize -> checked mutable
        // view. Returns invalid_argument when the buffer traps trip.
        [[nodiscard]] Expected<mem::MutableBufferView> acquireJobBuffer_(
            mem::UniqueBuffer &job, const std::size_t size_bytes) noexcept {
            job = mem::UniqueBuffer::allocate(size_bytes);
            if (const std::error_code err = job.materialize()) [[unlikely]] {
                return std::unexpected{err};
            }
            if (not job.isMaterialized()) [[unlikely]] {
                return std::unexpected{make_error_code(errc::invalid_argument)};
            }
            Expected<mem::MutableBufferView> view = job.getMutableData();
            if (not view.has_value() or view->size() != size_bytes) [[unlikely]] {
                return std::unexpected{make_error_code(errc::invalid_argument)};
            }
            return view;
        }

        // Freeze a filled job buffer: propagate the moveToShared error, then verify
        // the frozen view is materialized and complete (SharedBuffer has no
        // materialize() observer beyond isMaterialized()).
        [[nodiscard]] Expected<mem::SharedBuffer> freezeJobBuffer_(
            mem::UniqueBuffer &job, const std::size_t size_bytes) noexcept {
            mem::SharedBuffer frozen{};
            if (const std::error_code err = job.moveToShared(&frozen)) [[unlikely]] {
                return std::unexpected{err};
            }
            if (not frozen.isMaterialized() or frozen.getBufferData().size() != size_bytes) [[unlikely]] {
                return std::unexpected{make_error_code(errc::invalid_argument)};
            }
            return frozen;
        }

        [[nodiscard]] bool checkInvariants_(const ImageAsset &asset) noexcept {
            if (asset.m_dimension != ImageDimension::image2d) {
                return false;
            }
            if (asset.m_is_block != (asset.m_tag != BlockTag::none)) {
                return false;
            }
            if (asset.m_is_srgb != isSrgb(asset.m_format)) {
                return false;
            }
            if (asset.m_tag != blockTagOf(asset.m_format)) {
                return false;
            }
            if (not asset.m_is_block) {
                if (asset.m_block_w != 1u or asset.m_block_h != 1u or asset.m_bytes_per_block != 4u) {
                    return false;
                }
            } else {
                if (asset.m_block_w != blockWidthOf(asset.m_tag) or asset.m_block_h != blockHeightOf(asset.m_tag) or
                    asset.m_bytes_per_block != bytesPerBlockOf(asset.m_tag)) {
                    return false;
                }
            }
            if (asset.m_subresources.size() != asset.m_mip_count) {
                return false;
            }
            for (const ImageSubresource &sub: asset.m_subresources) {
                if (sub.m_row_pitch != rowPitchFor(asset.m_width, asset.m_tag)) {
                    return false;
                }
                if (sub.m_slice_pitch != slicePitchFor(asset.m_width, asset.m_height, asset.m_tag)) {
                    return false;
                }
                if (sub.m_view.getBufferData().size() != static_cast<std::size_t>(sub.m_slice_pitch)) {
                    return false;
                }
            }
            return true;
        }

        // Positive-pitch CPU Y-flip (sampler order); negative-stride Surface views
        // stay CPU-blit-only and never reach uploads.
        [[nodiscard]] std::error_code flipRows_(const mem::MutableBufferView pixels, const u64 row_pitch, const u32 height) {
            if (height <= 1u) {
                return default_value_v;
            }
            mem::UniqueBuffer row = mem::UniqueBuffer::scratch(static_cast<std::size_t>(row_pitch));
            Expected<mem::MutableBufferView> tmp = acquireJobBuffer_(row, static_cast<std::size_t>(row_pitch));
            if (not tmp.has_value()) {
                return tmp.error();
            }
            const std::size_t stride = static_cast<std::size_t>(row_pitch);
            for (u32 y = 0u; y < height / 2u; ++y) {
                std::byte *const top = pixels.data() + static_cast<std::size_t>(y) * stride;
                std::byte *const bottom = pixels.data() + static_cast<std::size_t>(height - 1u - y) * stride;
                std::ranges::copy(std::span<const std::byte>{top, stride}, tmp->begin());
                std::ranges::copy(std::span<const std::byte>{bottom, stride}, top);
                std::ranges::copy(*tmp, bottom);
            }
            return default_value_v;
        }
    } // namespace

    [[nodiscard]] Expected<ImageAsset> decodeToRgba8(
        const mem::SharedBufferView bytes, const std::string_view ext, const ImageDecodeDesc desc, const ImageUsage usage) {
        const std::string dotted = normalizeExtension_(ext);
        if (not isRgbaSource_(dotted)) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (bytes.empty()) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // Parser-safety gates before decoder construction (see above):
        // below-floor inputs and unsound PNG chains would over-read.
        if (bytes.size_bytes() < minInputBytesFor_(dotted)) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (dotted == ".png" and
            not validatePngChunks_({bytes.data(), bytes.size_bytes()})) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // Production limits first: bound header-parse work before the decoder
        // runs, then header claims before any allocation (fail-closed,
        // invalid_argument — never a throw, never a partial asset).
        if (bytes.size_bytes() > desc.m_limits.m_max_input_bytes) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }

        const mango::ConstMemory mango_mem{
            reinterpret_cast<const mango::u8 *>(bytes.data()), bytes.size_bytes()
        };
        mango::image::ImageDecoder decoder{mango_mem, "memory" + dotted};
        if (not decoder.isDecoder()) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const mango::image::ImageHeader header = decoder.header();
        if (header.width <= 0 or header.height <= 0) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (header.depth > 1 or header.faces > 1) [[unlikely]] {
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (static_cast<u64>(header.width) > desc.m_limits.m_max_width or
            static_cast<u64>(header.height) > desc.m_limits.m_max_height) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // sRGB from the header (!header.linear); usage data forces linear.
        const bool is_srgb = usage == ImageUsage::color ? not header.linear : false;
        const u32 width = static_cast<u32>(header.width);
        const u32 height = static_cast<u32>(header.height);
        const u64 row_pitch = rowPitchFor(width, BlockTag::none);
        // u64 compare before narrowing: closes overflow as well as over-limit.
        if (row_pitch * static_cast<u64>(height) > desc.m_limits.m_max_decoded_bytes) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const auto size_bytes = static_cast<std::size_t>(row_pitch * height);

        // One Mango decoder AND one UniqueBuffer per job: decode stages are
        // reentrant and const-thread-safe; frozen SharedBuffers share by value.
        mem::UniqueBuffer job{};
        Expected<mem::MutableBufferView> target = acquireJobBuffer_(job, size_bytes);
        if (not target.has_value()) [[unlikely]] {
            return std::unexpected{target.error()};
        }

        mango::image::ImageDecodeOptions options{};
        options.simd = desc.m_simd;
        options.multithread = false;
        const mango::image::Surface surface{
            header.width, header.height, rgba8Format_(), static_cast<std::size_t>(row_pitch), target->data()
        };
        const bool decoded = [&] {
            try {
                return static_cast<bool>(decoder.decode(surface, options));
            } catch (const std::exception &) {
                return false;
            } catch (...) {
                return false;
            }
        }();
        if (not decoded) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (desc.m_flip_v) {
            if (const std::error_code err = flipRows_(*target, row_pitch, height)) [[unlikely]] {
                return std::unexpected{err};
            }
        }

        Expected<mem::SharedBuffer> frozen = freezeJobBuffer_(job, size_bytes);
        if (not frozen.has_value()) [[unlikely]] {
            return std::unexpected{frozen.error()};
        }

        ImageAsset asset{};
        asset.m_width = width;
        asset.m_height = height;
        asset.m_mip_count = 1u;
        asset.m_dimension = ImageDimension::image2d;
        asset.m_format = is_srgb ? NativeImageFormat::rgba8_srgb : NativeImageFormat::rgba8_linear;
        asset.m_tag = BlockTag::none;
        asset.m_block_w = 1u;
        asset.m_block_h = 1u;
        asset.m_bytes_per_block = 4u;
        asset.m_is_srgb = is_srgb;
        asset.m_is_block = false;
        asset.m_storage = *frozen;
        asset.m_subresources.push_back(ImageSubresource{
            .m_view = asset.m_storage.subspan(0u, size_bytes),
            .m_row_pitch = row_pitch,
            .m_slice_pitch = static_cast<u64>(size_bytes),
        });

        PPR_ASSERT(checkInvariants_(asset));
        if (not checkInvariants_(asset)) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        return asset;
    }

    [[nodiscard]] Expected<ImageAsset> decodeToBlocks(
        const mem::SharedBufferView bytes, const std::string_view ext, const BlockTag want, const ImageDecodeDesc desc) {
        if (want == BlockTag::none) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const std::string dotted = normalizeExtension_(ext);
        if (not isRgbaSource_(dotted)) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // Passthrough/transcode only: PNG/JPG are never recompressed into blocks.
        if (not isCompressedSource_(dotted)) [[unlikely]] {
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (bytes.empty()) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (bytes.size_bytes() < minInputBytesFor_(dotted)) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (dotted == ".png" and
            not validatePngChunks_({bytes.data(), bytes.size_bytes()})) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (bytes.size_bytes() > desc.m_limits.m_max_input_bytes) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }

        const mango::ConstMemory mango_mem{
            reinterpret_cast<const mango::u8 *>(bytes.data()), bytes.size_bytes()
        };
        mango::image::ImageDecoder decoder{mango_mem, "memory" + dotted};
        if (not decoder.isDecoder()) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const mango::image::ImageHeader header = decoder.header();
        if (header.width <= 0 or header.height <= 0) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (header.depth > 1 or header.faces > 1) [[unlikely]] {
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (static_cast<u64>(header.width) > desc.m_limits.m_max_width or
            static_cast<u64>(header.height) > desc.m_limits.m_max_height) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }

        const bool is_srgb = not header.linear;
        const u32 mango_compression = mangoCompressionFor_(want, is_srgb);
        if (mango_compression == mango::image::TextureCompression::NONE) [[unlikely]] {
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        // Already-blocked sources (DDS natives) only serve their own blocks: DDS
        // ignores the transcode target and returns native bytes, so a differing
        // target is rejected here, never silently reinterpreted. Uncompressed
        // DDS has no blocks to pass through. KTX2 Basis (compression NONE) falls
        // through to the transcode attempt below.
        if (dotted == ".dds" and header.compression == mango::image::TextureCompression::NONE) [[unlikely]] {
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (header.compression != mango::image::TextureCompression::NONE and
            header.compression != mango_compression) [[unlikely]] {
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }

        mango::image::ImageDecodeOptions options{};
        options.simd = desc.m_simd;
        options.multithread = false;
        options.compression = mango_compression;

        // KTX2 keeps a single-slot transcode cache per decoder: query under a
        // shared mutex and clone immediately while the decoder lives.
        static std::mutex g_transcode_mutex{};
        mem::SharedBufferView blob_view{};
        {
            const std::lock_guard<std::mutex> lock{g_transcode_mutex};
            mango::ConstMemory blob_storage{};
            try {
                blob_storage = decoder.memory(0, 0, 0, options);
            } catch (const std::exception &) {
                return std::unexpected{make_error_code(errc::function_not_supported)};
            } catch (...) {
                return std::unexpected{make_error_code(errc::function_not_supported)};
            }
            if (blob_storage.size == 0u) [[unlikely]] {
                return std::unexpected{make_error_code(errc::function_not_supported)};
            }
            blob_view = mem::SharedBufferView{
                reinterpret_cast<const std::byte *>(blob_storage.address), blob_storage.size
            };
        }

        const u32 width = static_cast<u32>(header.width);
        const u32 height = static_cast<u32>(header.height);
        const u64 row_pitch = rowPitchFor(width, want);
        const u64 slice_bytes = slicePitchFor(width, height, want);
        if (slice_bytes > desc.m_limits.m_max_decoded_bytes) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const auto size_bytes = static_cast<std::size_t>(slice_bytes);
        if (blob_view.size() < size_bytes) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }

        mem::UniqueBuffer job{};
        Expected<mem::MutableBufferView> target = acquireJobBuffer_(job, size_bytes);
        if (not target.has_value()) [[unlikely]] {
            return std::unexpected{target.error()};
        }
        std::ranges::copy(blob_view.subspan(0u, size_bytes), target->begin());

        Expected<mem::SharedBuffer> frozen = freezeJobBuffer_(job, size_bytes);
        if (not frozen.has_value()) [[unlikely]] {
            return std::unexpected{frozen.error()};
        }

        ImageAsset asset{};
        asset.m_width = width;
        asset.m_height = height;
        asset.m_mip_count = 1u;
        asset.m_dimension = ImageDimension::image2d;
        asset.m_format = nativeFormatFor_(want, is_srgb);
        asset.m_tag = want;
        asset.m_block_w = blockWidthOf(want);
        asset.m_block_h = blockHeightOf(want);
        asset.m_bytes_per_block = bytesPerBlockOf(want);
        asset.m_is_srgb = is_srgb;
        asset.m_is_block = true;
        asset.m_storage = *frozen;
        asset.m_subresources.push_back(ImageSubresource{
            .m_view = asset.m_storage.subspan(0u, size_bytes),
            .m_row_pitch = row_pitch,
            .m_slice_pitch = static_cast<u64>(size_bytes),
        });

        PPR_ASSERT(checkInvariants_(asset));
        if (not checkInvariants_(asset)) [[unlikely]] {
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        return asset;
    }
}
