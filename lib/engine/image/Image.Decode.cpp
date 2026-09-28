module;
#include "pP/Macros.h"

#include <mango/image/image.hpp>

module engine.image;

import :types;
import :decode;
import :mips;
import engine.core;
import engine.math;

import std;

namespace pP::image {
    PPR_DECLARE_LOG_CATEGORY(Image)

    namespace {
        // ------------------------------------------------------------------
        // source validation
        // ------------------------------------------------------------------

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

        // ------------------------------------------------------------------
        // parser safety
        // ------------------------------------------------------------------

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

        // ------------------------------------------------------------------
        // mango format mapping
        // ------------------------------------------------------------------

        [[nodiscard]] mango::image::Format rgba8Format_() {
            using mango::image::Format;
            return Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8);
        }

        [[nodiscard]] u32 mangoCompressionFor_(const EBlockTag want, const bool is_srgb) noexcept {
            using mango::image::TextureCompression;
            switch (want) {
                case EBlockTag::bc1: return is_srgb ? TextureCompression::DXT1_SRGB : TextureCompression::DXT1;
                case EBlockTag::bc3: return is_srgb ? TextureCompression::DXT5_SRGB : TextureCompression::DXT5;
                case EBlockTag::bc4: return TextureCompression::RGTC1_RED;
                case EBlockTag::bc5: return TextureCompression::RGTC2_RG;
                case EBlockTag::bc7:
                    return is_srgb ? TextureCompression::BPTC_SRGB_ALPHA_UNORM : TextureCompression::BPTC_RGBA_UNORM;
                case EBlockTag::astc4x4: return is_srgb ? TextureCompression::ASTC_SRGB_4x4 : TextureCompression::ASTC_UNORM_4x4;
                case EBlockTag::astc6x6: return is_srgb ? TextureCompression::ASTC_SRGB_6x6 : TextureCompression::ASTC_UNORM_6x6;
                case EBlockTag::astc8x8: return is_srgb ? TextureCompression::ASTC_SRGB_8x8 : TextureCompression::ASTC_UNORM_8x8;
                default: return TextureCompression::NONE;
            }
        }

        [[nodiscard]] ENativeImageFormat nativeFormatFor_(const EBlockTag want, const bool is_srgb) noexcept {
            switch (want) {
                case EBlockTag::bc1: return is_srgb ? ENativeImageFormat::bc1_srgb : ENativeImageFormat::bc1_linear;
                case EBlockTag::bc3: return is_srgb ? ENativeImageFormat::bc3_srgb : ENativeImageFormat::bc3_linear;
                case EBlockTag::bc4: return ENativeImageFormat::bc4_linear;
                case EBlockTag::bc5: return ENativeImageFormat::bc5_linear;
                case EBlockTag::bc7: return is_srgb ? ENativeImageFormat::bc7_srgb : ENativeImageFormat::bc7_linear;
                case EBlockTag::astc4x4:
                    return is_srgb ? ENativeImageFormat::astc4x4_srgb : ENativeImageFormat::astc4x4_linear;
                case EBlockTag::astc6x6:
                    return is_srgb ? ENativeImageFormat::astc6x6_srgb : ENativeImageFormat::astc6x6_linear;
                case EBlockTag::astc8x8:
                    return is_srgb ? ENativeImageFormat::astc8x8_srgb : ENativeImageFormat::astc8x8_linear;
                default: return ENativeImageFormat::rgba8_linear;
            }
        }

        // ------------------------------------------------------------------
        // image buffer and invariant helpers
        // ------------------------------------------------------------------

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
            if (asset.m_dimension != EImageDimension::image2d) {
                return false;
            }
            if (asset.m_is_block != (asset.m_tag != EBlockTag::none)) {
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
            // Per-level pitches: single-level decodes check level 0, generated
            // chains check every halved extent (mip chain design, :mips).
            for (u32 level = 0u; level < asset.m_mip_count; ++level) {
                const u32 level_w = mipExtentAt(asset.m_width, level);
                const u32 level_h = mipExtentAt(asset.m_height, level);
                const ImageSubresource &sub = asset.m_subresources[level];
                if (sub.m_row_pitch != rowPitchFor(level_w, asset.m_tag)) {
                    return false;
                }
                if (sub.m_slice_pitch != slicePitchFor(level_w, level_h, asset.m_tag)) {
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
            const auto stride = static_cast<std::size_t>(row_pitch);
            mem::UniqueBuffer row = mem::UniqueBuffer::scratch(stride);
            Expected<mem::MutableBufferView> tmp = acquireJobBuffer_(row, stride);
            if (not tmp.has_value()) {
                return tmp.error();
            }
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

    // ------------------------------------------------------------------
    // rgba8 decoding
    // ------------------------------------------------------------------

    [[nodiscard]] Expected<ImageAsset> decodeToRgba8(
        const mem::SharedBufferView bytes, const std::string_view ext, const ImageDecodeDesc desc, const EImageUsage usage) {
        const std::string dotted = normalizeExtension_(ext);
        if (not isRgbaSource_(dotted)) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: unsupported extension", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (bytes.empty()) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: empty input", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // Parser-safety gates before decoder construction (see above):
        // below-floor inputs and unsound PNG chains would over-read.
        if (bytes.size_bytes() < minInputBytesFor_(dotted)) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: input below parser-safety floor",
                {{"ext", dotted}, {"bytes", static_cast<u64>(bytes.size_bytes())}, {"floor", static_cast<u64>(minInputBytesFor_(dotted))}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (dotted == ".png" and
            not validatePngChunks_({bytes.data(), bytes.size_bytes()})) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: unsound PNG chunk chain",
                {{"bytes", static_cast<u64>(bytes.size_bytes())}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // Production limits first: bound header-parse work before the decoder
        // runs, then header claims before any allocation (fail-closed,
        // invalid_argument — never a throw, never a partial asset).
        if (bytes.size_bytes() > desc.m_limits.m_max_input_bytes) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: input exceeds production cap",
                {{"ext", dotted}, {"bytes", static_cast<u64>(bytes.size_bytes())}, {"cap", desc.m_limits.m_max_input_bytes}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }

        const mango::ConstMemory mango_mem{
            reinterpret_cast<const mango::u8 *>(bytes.data()), bytes.size_bytes()
        };
        mango::image::ImageDecoder decoder{mango_mem, "memory" + dotted};
        if (not decoder.isDecoder()) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: no decoder for extension", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const mango::image::ImageHeader header = decoder.header();
        if (header.width <= 0 or header.height <= 0) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: non-positive header extents",
                {{"ext", dotted}, {"width", header.width}, {"height", header.height}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (header.depth > 1 or header.faces > 1) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: volume or face array is deferred", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (static_cast<u64>(header.width) > desc.m_limits.m_max_width or
            static_cast<u64>(header.height) > desc.m_limits.m_max_height) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: header exceeds dimension caps",
                {{"ext", dotted}, {"width", static_cast<u64>(header.width)}, {"height", static_cast<u64>(header.height)}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // sRGB from the header (!header.linear); usage data forces linear.
        const bool is_srgb = usage == EImageUsage::color ? not header.linear : false;
        if (usage == EImageUsage::data and not header.linear) {
            PPR_LOG(Image, info, "decode coerces sRGB source to linear for data usage",
                {{"ext", dotted}, {"forced_linear", true}});
        }

        const u32 width = static_cast<u32>(header.width);
        const u32 height = static_cast<u32>(header.height);
        const u64 row_pitch = rowPitchFor(width, EBlockTag::none);

        // u64 compare before narrowing: closes overflow as well as over-limit.
        if (row_pitch * static_cast<u64>(height) > desc.m_limits.m_max_decoded_bytes) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: unpacked bytes exceed production cap",
                {{"ext", dotted}, {"bytes", row_pitch * static_cast<u64>(height)}, {"cap", desc.m_limits.m_max_decoded_bytes}});
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
        options.simd = desc.m_use_simd;
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
            PPR_LOG_ONCE(Image, warning, "decode rejected: mango decoder failed", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (desc.m_use_flip_v) {
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
        asset.m_dimension = EImageDimension::image2d;
        asset.m_format = is_srgb ? ENativeImageFormat::rgba8_srgb : ENativeImageFormat::rgba8_linear;
        asset.m_tag = EBlockTag::none;
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
            PPR_LOG_ONCE(Image, warning, "decode rejected: asset invariant failed", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        return asset;
    }

    // ------------------------------------------------------------------
    // block decoding
    // ------------------------------------------------------------------

    [[nodiscard]] Expected<ImageAsset> decodeToBlocks(
        const mem::SharedBufferView bytes, const std::string_view ext, const EBlockTag want, const ImageDecodeDesc desc) {
        if (want == EBlockTag::none) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: block target is none", {{"ext", ext}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const std::string dotted = normalizeExtension_(ext);
        if (not isRgbaSource_(dotted)) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: unsupported extension", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        // Passthrough/transcode only: PNG/JPG are never recompressed into blocks.
        if (not isCompressedSource_(dotted)) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: block target needs a compressed source", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (bytes.empty()) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: empty input", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (bytes.size_bytes() < minInputBytesFor_(dotted)) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: input below parser-safety floor",
                {{"ext", dotted}, {"bytes", static_cast<u64>(bytes.size_bytes())}, {"floor", static_cast<u64>(minInputBytesFor_(dotted))}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (dotted == ".png" and
            not validatePngChunks_({bytes.data(), bytes.size_bytes()})) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: unsound PNG chunk chain",
                {{"bytes", static_cast<u64>(bytes.size_bytes())}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (bytes.size_bytes() > desc.m_limits.m_max_input_bytes) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: input exceeds production cap",
                {{"ext", dotted}, {"bytes", static_cast<u64>(bytes.size_bytes())}, {"cap", desc.m_limits.m_max_input_bytes}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }

        const mango::ConstMemory mango_mem{
            reinterpret_cast<const mango::u8 *>(bytes.data()), bytes.size_bytes()
        };
        mango::image::ImageDecoder decoder{mango_mem, "memory" + dotted};
        if (not decoder.isDecoder()) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: no decoder for extension", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const mango::image::ImageHeader header = decoder.header();
        if (header.width <= 0 or header.height <= 0) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: non-positive header extents",
                {{"ext", dotted}, {"width", header.width}, {"height", header.height}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        if (header.depth > 1 or header.faces > 1) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: volume or face array is deferred", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (static_cast<u64>(header.width) > desc.m_limits.m_max_width or
            static_cast<u64>(header.height) > desc.m_limits.m_max_height) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: header exceeds dimension caps",
                {{"ext", dotted}, {"width", static_cast<u64>(header.width)}, {"height", static_cast<u64>(header.height)}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }

        const bool is_srgb = not header.linear;
        const u32 mango_compression = mangoCompressionFor_(want, is_srgb);
        if (mango_compression == mango::image::TextureCompression::NONE) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: no block mapping for target", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        // Already-blocked sources (DDS natives) only serve their own blocks: DDS
        // ignores the transcode target and returns native bytes, so a differing
        // target is rejected here, never silently reinterpreted. Uncompressed
        // DDS has no blocks to pass through. KTX2 Basis (compression NONE) falls
        // through to the transcode attempt below.
        if (dotted == ".dds" and header.compression == mango::image::TextureCompression::NONE) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: uncompressed DDS has no blocks to serve", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (header.compression != mango::image::TextureCompression::NONE and
            header.compression != mango_compression) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: block target differs from native storage", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }

        mango::image::ImageDecodeOptions options{};
        options.simd = desc.m_use_simd;
        options.multithread = false;
        options.compression = mango_compression;

        // KTX2 single-slot transcode cache is per-decoder (Mango's Interface owns
        // its transcode buffer; each job owns its decoder) and Mango's only shared
        // transcode state (basisu one-time init) guards itself: the transcode runs
        // unlocked so concurrent jobs parallelize. Clone immediately while the
        // decoder lives; the frozen copy outlives it.
        mango::ConstMemory blob_storage{};
        try {
            blob_storage = decoder.memory(0, 0, 0, options);
        } catch (const std::exception &) {
            PPR_LOG_ONCE(Image, warning, "decode rejected: block transcode failed", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        } catch (...) {
            PPR_LOG_ONCE(Image, warning, "decode rejected: block transcode failed", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        if (blob_storage.size == 0u) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: block transcode produced no bytes", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::function_not_supported)};
        }
        const mem::SharedBufferView blob_view{
            reinterpret_cast<const std::byte *>(blob_storage.address), blob_storage.size
        };

        const u32 width = static_cast<u32>(header.width);
        const u32 height = static_cast<u32>(header.height);
        const u64 row_pitch = rowPitchFor(width, want);
        const u64 slice_bytes = slicePitchFor(width, height, want);
        if (slice_bytes > desc.m_limits.m_max_decoded_bytes) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: unpacked bytes exceed production cap",
                {{"ext", dotted}, {"bytes", slice_bytes}, {"cap", desc.m_limits.m_max_decoded_bytes}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        const auto size_bytes = static_cast<std::size_t>(slice_bytes);
        // Exact size: a short level is truncated input while a long one is not
        // the requested blocks (uncompressed sources serve raw bytes here) —
        // both reject fail-closed, never reinterpreted.
        if (blob_view.size() != size_bytes) [[unlikely]] {
            PPR_LOG_ONCE(Image, warning, "decode rejected: block bytes differ from requested size",
                {{"ext", dotted}, {"bytes", static_cast<u64>(blob_view.size())}, {"want", static_cast<u64>(size_bytes)}});
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
        asset.m_dimension = EImageDimension::image2d;
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
            PPR_LOG_ONCE(Image, warning, "decode rejected: asset invariant failed", {{"ext", dotted}});
            return std::unexpected{make_error_code(errc::invalid_argument)};
        }
        return asset;
    }
}
