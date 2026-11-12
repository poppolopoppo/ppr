// ------------------------------------------------------------------
// shared exact IEEE binary16 codec
// ------------------------------------------------------------------
//
// Internal helper, not a public engine type: the resolved-cell/interval codecs
// in `:renderer.world_lighting` and the particle-presentation unit tests both
// need the same binary16 conversion, so it lives in one header rather than in a
// file-local copy per consumer. It is header-only and include-path independent,
// so the module implementation and the app test target can share it without a
// new module export.

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace pP::details {
    // Exact IEEE binary16 -> binary32 decode. Half subnormals are normalized
    // from their ten-bit significand; zero preserves its sign.
    [[nodiscard]] inline float decodeHalf(const std::uint16_t bits) noexcept {
        const std::uint32_t sign = (bits & 0x8000u) != 0u ? 0x8000'0000u : 0u;
        const std::uint32_t exponent = (bits >> 10u) & 0x1fu;
        std::uint32_t mantissa = bits & 0x3ffu;

        if (exponent == 0u) {
            if (mantissa == 0u) {
                return std::bit_cast<float>(sign);
            }

            std::uint32_t shift = 0u;
            while ((mantissa & 0x400u) == 0u) {
                mantissa <<= 1u;
                ++shift;
            }
            mantissa &= 0x3ffu;
            return std::bit_cast<float>(sign | ((113u - shift) << 23u) | (mantissa << 13u));
        }

        if (exponent == 31u) {
            return std::bit_cast<float>(sign | 0x7f80'0000u | (mantissa << 13u));
        }

        return std::bit_cast<float>(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
    }

    // Exact binary16 nearest/ties-to-even encode for a validated finite value
    // whose magnitude is at most 65,504; the caller owns domain rejection.
    // Negative zero canonicalizes to +0 and a magnitude that rounds past the
    // largest binary16 exponent saturates to infinity carrying the sign.
    [[nodiscard]] inline std::uint16_t encodeHalf(const double value) noexcept {
        if (value == 0.0) {
            return 0u;
        }

        const bool negative = value < 0.0;
        const double magnitude = negative ? -value : value;
        const std::uint16_t sign_bit = negative ? std::uint16_t{0x8000u} : std::uint16_t{0u};
        if (magnitude < 0x1p-14) {
            const double scaled = std::ldexp(magnitude, 24);
            const double base = std::floor(scaled);
            const double fraction = scaled - base;
            std::uint32_t quantized = static_cast<std::uint32_t>(base);
            if (fraction > 0.5 or (fraction == 0.5 and (quantized & 1u) != 0u)) {
                ++quantized;
            }

            // Rounding a subnormal up can reach the smallest normal.
            return quantized >= 1024u ? static_cast<std::uint16_t>(0x0400u | sign_bit)
                                      : static_cast<std::uint16_t>(quantized | sign_bit);
        }

        const int exponent = std::ilogb(magnitude);
        const double scaled = std::ldexp(magnitude, 10 - exponent);
        const double base = std::floor(scaled);
        const double fraction = scaled - base;
        std::uint32_t significand = static_cast<std::uint32_t>(base);
        if (fraction > 0.5 or (fraction == 0.5 and (significand & 1u) != 0u)) {
            ++significand;
        }

        int half_exponent = exponent;
        if (significand == 2048u) {
            significand = 1024u;
            ++half_exponent;
        }
        if (half_exponent > 15) {
            return static_cast<std::uint16_t>(0x7c00u | sign_bit);
        }

        return static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(half_exponent + 15) << 10u) | (significand - 1024u) | sign_bit);
    }
} // namespace pP::details
