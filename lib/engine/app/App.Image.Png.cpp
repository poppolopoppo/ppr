module;

#include "pP/Macros.h"

#include <zlib.h>

module engine.app;

import :image.png;
import :renderer;

import engine.core;
import engine.rhi;

import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(SurfacePng, info, none)

    // ------------------------------------------------------------------
    // PNG chunk encoding
    // ------------------------------------------------------------------

    namespace {
        constexpr std::array<std::byte, 8u> kPngSignature{
            std::byte{0x89},
            std::byte{'P'},
            std::byte{'N'},
            std::byte{'G'},
            std::byte{0x0D},
            std::byte{0x0A},
            std::byte{0x1A},
            std::byte{0x0A},
        };

        constexpr std::array<char, 4u> kIhdrType{'I', 'H', 'D', 'R'};
        constexpr std::array<char, 4u> kIdatType{'I', 'D', 'A', 'T'};
        constexpr std::array<char, 4u> kIendType{'I', 'E', 'N', 'D'};

        void appendU32BigEndian_(std::span<std::byte, 4u> out, const u32 value) noexcept {
            out[0] = static_cast<std::byte>((value >> 24u) & 0xFFu);
            out[1] = static_cast<std::byte>((value >> 16u) & 0xFFu);
            out[2] = static_cast<std::byte>((value >> 8u) & 0xFFu);
            out[3] = static_cast<std::byte>(value & 0xFFu);
        }

        /// Length + type + payload + CRC, the exact PNG chunk framing. The
        /// payload length is the stream's 31-bit length field; the zlib CRC
        /// covers the type and the payload (never the length).
        [[nodiscard]] std::error_code writeChunk_(
            std::ostream &stream, const std::array<char, 4u> &type, const std::span<const std::byte> payload) {
            PPR_RETURN_ERROR_ON_FAIL(
                SurfacePng,
                payload.size() <= 0x7FFFFFFFu ? std::error_code{} : std::make_error_code(std::errc::value_too_large));
            PPR_RETURN_ERROR_ON_FAIL(
                SurfacePng, payload.size() <= (std::numeric_limits<uInt>::max)()
                                ? std::error_code{}
                                : std::make_error_code(std::errc::value_too_large));

            std::array<std::byte, 4u> length{};
            appendU32BigEndian_(length, static_cast<u32>(payload.size()));

            uLong crc = crc32(0L, Z_NULL, 0u);
            crc = crc32(crc, reinterpret_cast<const Bytef *>(type.data()), static_cast<uInt>(type.size()));
            if (not payload.empty()) {
                crc = crc32(crc, reinterpret_cast<const Bytef *>(payload.data()), static_cast<uInt>(payload.size()));
            }

            std::array<std::byte, 4u> crc_bytes{};
            appendU32BigEndian_(crc_bytes, static_cast<u32>(crc));

            stream.write(reinterpret_cast<const char *>(length.data()), 4);
            stream.write(type.data(), 4);
            if (not payload.empty()) {
                stream.write(
                    reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
            }
            stream.write(reinterpret_cast<const char *>(crc_bytes.data()), 4);
            return stream ? std::error_code{} : std::make_error_code(std::errc::io_error);
        }

        /// Compresses the filtered scanline stream as a zlib stream, which is
        /// exactly the PNG IDAT payload (deflate with a zlib wrapper).
        [[nodiscard]] Expected<std::vector<std::byte>> compressScanlines_(const std::span<const std::byte> scanlines) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SurfacePng, scanlines.size() <= (std::numeric_limits<uLong>::max)()
                                ? std::error_code{}
                                : std::make_error_code(std::errc::value_too_large));

            const uLong bound = compressBound(static_cast<uLong>(scanlines.size()));
            std::vector<std::byte> compressed(static_cast<std::size_t>(bound));
            uLongf compressed_size = bound;
            const int status = compress2(
                reinterpret_cast<Bytef *>(compressed.data()), &compressed_size,
                reinterpret_cast<const Bytef *>(scanlines.data()), static_cast<uLong>(scanlines.size()),
                Z_DEFAULT_COMPRESSION);
            if (status == Z_MEM_ERROR) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(SurfacePng, std::make_error_code(std::errc::not_enough_memory));
            }
            if (status != Z_OK) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(SurfacePng, std::make_error_code(std::errc::state_not_recoverable));
            }

            compressed.resize(static_cast<std::size_t>(compressed_size));
            return compressed;
        }
    } // namespace

    // ------------------------------------------------------------------
    // surface readback to PNG
    // ------------------------------------------------------------------

    std::error_code writeSurfacePng(const SurfaceFramePixels &pixels, const std::filesystem::path &path) {
        const bool rgba = pixels.m_format == rhi::Format::RGBA8Unorm or pixels.m_format == rhi::Format::RGBA8UnormSrgb;
        const bool bgra = pixels.m_format == rhi::Format::BGRA8Unorm or pixels.m_format == rhi::Format::BGRA8UnormSrgb;
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng, (rgba or bgra) and pixels.m_extent.x > 0 and pixels.m_extent.y > 0
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        const u64 width = static_cast<u64>(pixels.m_extent.x);
        const u64 height = static_cast<u64>(pixels.m_extent.y);
        constexpr u64 kMax = (std::numeric_limits<u64>::max)();
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng, width <= kMax / 4u ? std::error_code{} : std::make_error_code(std::errc::value_too_large));
        const u64 row_bytes = width * 4u;
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng, pixels.m_row_pitch >= row_bytes and pixels.m_row_pitch <= kMax / height
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng, pixels.m_row_pitch * height <= static_cast<u64>(pixels.m_bytes.size())
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        // One filter byte (None) per scanline; row order is the readback's
        // own top-down order, so no vertical flip is applied.
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng,
            row_bytes < kMax / height ? std::error_code{} : std::make_error_code(std::errc::value_too_large));
        const u64 scanline_bytes = row_bytes + 1u;
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng,
            scanline_bytes <= kMax / height ? std::error_code{} : std::make_error_code(std::errc::value_too_large));
        const u64 scanlines_size = scanline_bytes * height;
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng, scanlines_size <= (std::numeric_limits<std::size_t>::max)()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::value_too_large));

        std::vector<std::byte> scanlines(static_cast<std::size_t>(scanlines_size));
        for (u64 row = 0u; row < height; ++row) {
            std::byte *const destination = scanlines.data() + static_cast<std::size_t>(row * scanline_bytes);
            destination[0] = std::byte{0u};

            const std::byte *const source = pixels.m_bytes.data() + static_cast<std::size_t>(row * pixels.m_row_pitch);
            if (rgba) {
                std::memcpy(destination + 1u, source, static_cast<std::size_t>(row_bytes));
                continue;
            }

            // BGRA source: swap red and blue, preserve green and alpha.
            for (u64 x = 0u; x < width; ++x) {
                const std::byte *const pixel = source + static_cast<std::size_t>(x * 4u);
                std::byte *const out = destination + 1u + static_cast<std::size_t>(x * 4u);
                out[0] = pixel[2];
                out[1] = pixel[1];
                out[2] = pixel[0];
                out[3] = pixel[3];
            }
        }

        Expected<std::vector<std::byte>> compressed = compressScanlines_(scanlines);
        PPR_RETURN_ERROR_ON_FAIL(SurfacePng, compressed);

        std::array<std::byte, 13u> ihdr{};
        appendU32BigEndian_(std::span<std::byte, 4u>{ihdr.data(), 4u}, static_cast<u32>(width));
        appendU32BigEndian_(std::span<std::byte, 4u>{ihdr.data() + 4u, 4u}, static_cast<u32>(height));
        ihdr[8] = std::byte{8u}; // bit depth
        ihdr[9] = std::byte{6u}; // colour type RGBA
        ihdr[10] = std::byte{0u}; // compression: deflate
        ihdr[11] = std::byte{0u}; // filter method: adaptive
        ihdr[12] = std::byte{0u}; // interlace: none

        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        PPR_RETURN_ERROR_ON_FAIL(SurfacePng, stream ? std::error_code{} : std::make_error_code(std::errc::io_error));

        bool written = false;
        PPR_DEFER {
            stream.close();
            if (not written) {
                std::error_code remove_error{};
                std::filesystem::remove(path, remove_error);
            }
        };

        stream.write(reinterpret_cast<const char *>(kPngSignature.data()), 8);
        PPR_RETURN_ERROR_ON_FAIL(SurfacePng, stream ? std::error_code{} : std::make_error_code(std::errc::io_error));

        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng, writeChunk_(stream, kIhdrType, std::span<const std::byte>{ihdr.data(), ihdr.size()}));
        PPR_RETURN_ERROR_ON_FAIL(
            SurfacePng,
            writeChunk_(stream, kIdatType, std::span<const std::byte>{compressed->data(), compressed->size()}));
        PPR_RETURN_ERROR_ON_FAIL(SurfacePng, writeChunk_(stream, kIendType, std::span<const std::byte>{}));

        stream.close();
        PPR_RETURN_ERROR_ON_FAIL(SurfacePng, stream ? std::error_code{} : std::make_error_code(std::errc::io_error));

        written = true;
        return {};
    }
} // namespace pP
