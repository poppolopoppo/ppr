module;
#include "pP/Macros.h"
module engine.app;

import :renderer.world_lighting;

import engine.core;
import engine.math;
import std;

namespace pP::lighting {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(WorldLighting, info, none)

    namespace {
        // ------------------------------------------------------------------
        // binary16 conversion
        // ------------------------------------------------------------------

        // Exact IEEE binary16 -> binary32 decode. Half subnormals are
        // normalized from their ten-bit significand; zero preserves its sign.
        [[nodiscard]] float decodeHalf(const u16 bits) noexcept {
            const u32 sign = (bits & 0x8000u) != 0u ? 0x8000'0000u : 0u;
            const u32 exponent = (bits >> 10u) & 0x1fu;
            u32 mantissa = bits & 0x3ffu;

            if (exponent == 0u) {
                if (mantissa == 0u) {
                    return std::bit_cast<float>(sign);
                }

                u32 shift = 0u;
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

        // Exact binary16 nearest/ties-to-even encode for a validated finite
        // value in [0, 65,504]. The caller owns domain rejection and the
        // negative-zero canonicalization.
        [[nodiscard]] u16 encodeHalf(const double value) noexcept {
            if (value == 0.0) {
                return 0u;
            }

            if (value < 0x1p-14) {
                const double scaled = std::ldexp(value, 24);
                const double base = std::floor(scaled);
                const double fraction = scaled - base;
                u32 quantized = static_cast<u32>(base);
                if (fraction > 0.5 or (fraction == 0.5 and (quantized & 1u) != 0u)) {
                    ++quantized;
                }

                // Rounding a subnormal up can reach the smallest normal.
                return quantized >= 1024u ? u16{0x0400u} : static_cast<u16>(quantized);
            }

            const int exponent = std::ilogb(value);
            const double scaled = std::ldexp(value, 10 - exponent);
            const double base = std::floor(scaled);
            const double fraction = scaled - base;
            u32 significand = static_cast<u32>(base);
            if (fraction > 0.5 or (fraction == 0.5 and (significand & 1u) != 0u)) {
                ++significand;
            }

            int half_exponent = exponent;
            if (significand == 2048u) {
                significand = 1024u;
                ++half_exponent;
            }
            if (half_exponent > 15) {
                return 0x7c00u;
            }

            return static_cast<u16>((static_cast<u32>(half_exponent + 15) << 10u) | (significand - 1024u));
        }

        // ------------------------------------------------------------------
        // shared admission checks
        // ------------------------------------------------------------------

        [[nodiscard]] bool finiteInRange(const float value, const float low, const float high) noexcept {
            return std::isfinite(value) and value >= low and value <= high;
        }

        [[nodiscard]] std::error_code invalidArgument() noexcept {
            return std::make_error_code(std::errc::invalid_argument);
        }

        [[nodiscard]] std::error_code outOfRange() noexcept {
            return std::make_error_code(std::errc::result_out_of_range);
        }

        struct RgbComponents {
            float m_r{};
            float m_g{};
            float m_b{};
        };

        [[nodiscard]] RgbComponents componentsOf(const float3 value) noexcept {
            return {value.x, value.y, value.z};
        }
    } // namespace

    // ------------------------------------------------------------------
    // resolved cell (material ID16 plus binary16 Kelvin)
    // ------------------------------------------------------------------

    Expected<u32> packCell(const u16 material, const double kelvin) {
        if (material == 0u) {
            return 0u;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldLighting, std::isfinite(kelvin) and kelvin >= 0.0 ? std::error_code{} : invalidArgument());
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLighting, kelvin <= 65'504.0 ? std::error_code{} : outOfRange());

        const u16 temperature = encodeHalf(kelvin == 0.0 ? 0.0 : kelvin);
        return (static_cast<u32>(temperature) << 16u) | material;
    }

    // ------------------------------------------------------------------
    // RGB9E5 shared-exponent radiance
    // ------------------------------------------------------------------

    Expected<u32> packRGB9E5(const float3 rgb) {
        const RgbComponents components = componentsOf(rgb);
        const std::array<float, 3u> channels{components.m_r, components.m_g, components.m_b};

        for (const float channel: channels) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldLighting, std::isfinite(channel) and channel >= 0.0f ? std::error_code{} : invalidArgument());
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLighting, channel <= 65'408.0f ? std::error_code{} : outOfRange());
        }

        if (channels[0] == 0.0f and channels[1] == 0.0f and channels[2] == 0.0f) {
            return 0u;
        }

        // Smallest shared exponent whose rounded mantissas fit in [0,511].
        // Rounding is nearest/ties-up for this unsigned codec.
        for (u32 exponent = 0u; exponent <= 31u; ++exponent) {
            std::array<u32, 3u> mantissas{};
            bool fits = true;
            for (u32 channel = 0u; channel < channels.size(); ++channel) {
                const double scaled = std::ldexp(
                    static_cast<double>(channels[channel]), 24 - static_cast<int>(exponent));
                const double rounded = std::floor(scaled + 0.5);
                if (rounded > 511.0) {
                    fits = false;
                    break;
                }
                mantissas[channel] = static_cast<u32>(rounded);
            }

            if (fits) {
                return mantissas[0] | (mantissas[1] << 9u) | (mantissas[2] << 18u) | (exponent << 27u);
            }
        }

        return std::unexpected{outOfRange()};
    }

    float3 unpackRGB9E5(const u32 word) noexcept {
        const int exponent = static_cast<int>((word >> 27u) & 0x1fu) - 24;
        return float3{
            std::ldexp(static_cast<float>(word & 0x1ffu), exponent),
            std::ldexp(static_cast<float>((word >> 9u) & 0x1ffu), exponent),
            std::ldexp(static_cast<float>((word >> 18u) & 0x1ffu), exponent)};
    }

    // ------------------------------------------------------------------
    // coarse interval
    // ------------------------------------------------------------------

    Expected<PackedInterval> packInterval(const Interval &value) {
        const RgbComponents radiance = componentsOf(value.m_radiance);
        for (const float channel: std::array{radiance.m_r, radiance.m_g, radiance.m_b}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldLighting, std::isfinite(channel) and channel >= 0.0f ? std::error_code{} : invalidArgument());
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLighting, channel <= 65'504.0f ? std::error_code{} : outOfRange());
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldLighting, std::isfinite(value.m_transmittance) and finiteInRange(value.m_transmittance, 0.0f, 1.0f)
                               ? std::error_code{}
                               : invalidArgument());

        PackedInterval packed{};
        packed.m_rg = static_cast<u32>(encodeHalf(static_cast<double>(radiance.m_r))) |
                      (static_cast<u32>(encodeHalf(static_cast<double>(radiance.m_g))) << 16u);
        packed.m_bt = static_cast<u32>(encodeHalf(static_cast<double>(radiance.m_b))) |
                      (static_cast<u32>(encodeHalf(static_cast<double>(value.m_transmittance))) << 16u);
        return packed;
    }

    Interval unpackInterval(const PackedInterval value) noexcept {
        Interval decoded{};
        decoded.m_radiance = float3{
            decodeHalf(static_cast<u16>(value.m_rg & 0xffffu)),
            decodeHalf(static_cast<u16>((value.m_rg >> 16u) & 0xffffu)),
            decodeHalf(static_cast<u16>(value.m_bt & 0xffffu))};
        decoded.m_transmittance = decodeHalf(static_cast<u16>((value.m_bt >> 16u) & 0xffffu));
        return decoded;
    }

    // ------------------------------------------------------------------
    // fine circular harmonics
    // ------------------------------------------------------------------

    Expected<PackedCH> packCH(const CH &value) {
        const Expected<u32> dc = packRGB9E5(value.m_coefficients[0]);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLighting, dc ? std::error_code{} : dc.error());

        float maximum = 0.0f;
        for (u32 coefficient = 1u; coefficient < value.m_coefficients.size(); ++coefficient) {
            const RgbComponents ac = componentsOf(value.m_coefficients[coefficient]);
            for (const float component: std::array{ac.m_r, ac.m_g, ac.m_b}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldLighting, std::isfinite(component) ? std::error_code{} : invalidArgument());
                maximum = (std::max) (maximum, std::abs(component));
            }
        }

        PackedCH packed{};
        packed.m_words[0] = *dc;
        if (maximum == 0.0f) {
            return packed;
        }

        const float scale = maximum / 127.0f;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLighting, std::isfinite(scale) ? std::error_code{} : invalidArgument());
        // A nonzero maximum whose FP32 scale underflows encodes zero AC; its
        // absolute error is below 127 times the smallest positive FP32 value.
        if (scale == 0.0f) {
            return packed;
        }
        packed.m_words[1] = std::bit_cast<u32>(scale);

        for (u32 coefficient_index = 1u; coefficient_index < value.m_coefficients.size(); ++coefficient_index) {
            const RgbComponents ac = componentsOf(value.m_coefficients[coefficient_index]);
            const std::array<float, 3u> components{ac.m_r, ac.m_g, ac.m_b};
            for (u32 channel = 0u; channel < components.size(); ++channel) {
                const float scaled = components[channel] / scale;
                const float base = std::floor(scaled);
                const float fraction = scaled - base;
                i32 quantized = static_cast<i32>(base);
                if (fraction > 0.5f or (fraction == 0.5f and (quantized & 1) != 0)) {
                    ++quantized;
                }
                quantized = (std::clamp) (quantized, -127, 127);

                const u32 component = (coefficient_index - 1u) * 3u + channel;
                packed.m_words[2u + component / 4u] |= static_cast<u32>(static_cast<u8>(quantized))
                                                       << ((component % 4u) * 8u);
            }
        }

        return packed;
    }

    CH unpackCH(const PackedCH &value) noexcept {
        CH decoded{};
        decoded.m_coefficients[0] = unpackRGB9E5(value.m_words[0]);
        const float scale = std::bit_cast<float>(value.m_words[1]);

        for (u32 coefficient = 0u; coefficient < 8u; ++coefficient) {
            std::array<float, 3u> components{};
            for (u32 channel = 0u; channel < components.size(); ++channel) {
                const u32 component = coefficient * 3u + channel;
                const u32 word = value.m_words[2u + component / 4u];
                const u8 encoded = static_cast<u8>((word >> ((component % 4u) * 8u)) & 0xffu);
                const i32 signed_value = encoded < 0x80u ? static_cast<i32>(encoded) : static_cast<i32>(encoded) - 256;
                components[channel] = static_cast<float>(signed_value) * scale;
            }
            decoded.m_coefficients[coefficient + 1u] = float3{components[0], components[1], components[2]};
        }

        return decoded;
    }

    // ------------------------------------------------------------------
    // eight-byte source-material palette record
    // ------------------------------------------------------------------

    Expected<SourceMaterial> makeSourceMaterial(
        const float3 emission, const double strength, const float3 tint, const float3 texture_mean,
        const float metallic) {
        const RgbComponents emission_components = componentsOf(emission);
        const RgbComponents tint_components = componentsOf(tint);
        const RgbComponents mean_components = componentsOf(texture_mean);

        for (const float component:
            std::array{emission_components.m_r, emission_components.m_g, emission_components.m_b}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldLighting, std::isfinite(component) and component >= 0.0f ? std::error_code{} : invalidArgument());
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldLighting, std::isfinite(strength) and strength >= 0.0 ? std::error_code{} : invalidArgument());

        for (const float component: std::array{
                 tint_components.m_r, tint_components.m_g, tint_components.m_b, mean_components.m_r,
                 mean_components.m_g, mean_components.m_b}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldLighting, finiteInRange(component, 0.0f, 1.0f) ? std::error_code{} : invalidArgument());
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldLighting, finiteInRange(metallic, 0.0f, 1.0f) ? std::error_code{} : invalidArgument());

        // Checked binary64 authored-emission multiplication before encoding.
        const std::array<double, 3u> emission_channels{
            static_cast<double>(emission_components.m_r), static_cast<double>(emission_components.m_g),
            static_cast<double>(emission_components.m_b)};
        std::array<float, 3u> authored{};
        for (u32 channel = 0u; channel < authored.size(); ++channel) {
            const double product = emission_channels[channel] * strength;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldLighting, std::isfinite(product) and product <= 65'408.0 ? std::error_code{} : outOfRange());
            authored[channel] = static_cast<float>(product);
        }

        const double diffuse_factor = 1.0 - static_cast<double>(metallic);
        const std::array<float, 3u> diffuse{
            static_cast<float>(
                static_cast<double>(tint_components.m_r) * static_cast<double>(mean_components.m_r) * diffuse_factor),
            static_cast<float>(
                static_cast<double>(tint_components.m_g) * static_cast<double>(mean_components.m_g) * diffuse_factor),
            static_cast<float>(
                static_cast<double>(tint_components.m_b) * static_cast<double>(mean_components.m_b) * diffuse_factor)};

        const Expected<u32> emission_word = packRGB9E5(float3{authored[0], authored[1], authored[2]});
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLighting, emission_word ? std::error_code{} : emission_word.error());
        const Expected<u32> diffuse_word = packRGB9E5(float3{diffuse[0], diffuse[1], diffuse[2]});
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLighting, diffuse_word ? std::error_code{} : diffuse_word.error());

        return SourceMaterial{*emission_word, *diffuse_word};
    }

    // ------------------------------------------------------------------
    // retained direction identity
    // ------------------------------------------------------------------

    std::optional<u64> lastTraceFrame(const u64 accepted_frame, const u32 group_phase, const u32 local_probe) noexcept {
        if (group_phase > 3u or local_probe > 3u) {
            return std::nullopt;
        }

        const u32 age = (static_cast<u32>(accepted_frame & 3u) + group_phase + 4u - local_probe) & 3u;
        if (accepted_frame < age) {
            return std::nullopt;
        }

        return accepted_frame - age;
    }

    // ------------------------------------------------------------------
    // native-cell traversal reference
    // ------------------------------------------------------------------

    std::optional<u32> traceNativeCellDDA(
        const std::span<const u8> occupancy, const u32 edge, const float2 start, const float2 end) noexcept {
        if (edge == 0u or occupancy.size() < static_cast<std::size_t>(edge) * edge) {
            return std::nullopt;
        }

        if (not std::isfinite(start.x) or
            not std::isfinite(start.y) or
            not std::isfinite(end.x) or
            not std::isfinite(end.y)) {
            return std::nullopt;
        }

        const float2 delta{end.x - start.x, end.y - start.y};
        if (not std::isfinite(delta.x) or not std::isfinite(delta.y)) {
            return std::nullopt;
        }

        // A zero-length segment is a point, not a positive-length intersection.
        if (delta.x == 0.0f and delta.y == 0.0f) {
            return std::nullopt;
        }

        // Slab-clip the finite segment to [0, edge) in both axes. A zero
        // direction component is a parallel slab: it either lies inside the
        // domain on that axis or the whole segment is exterior.
        const float edge_extent = static_cast<float>(edge);
        float enter = 0.0f;
        float exit = 1.0f;
        for (const u32 axis: {0u, 1u}) {
            const float origin = axis == 0u ? start.x : start.y;
            const float direction = axis == 0u ? delta.x : delta.y;
            if (direction == 0.0f) {
                if (origin < 0.0f or origin >= edge_extent) {
                    return std::nullopt;
                }

                continue;
            }

            const float lower = (0.0f - origin) / direction;
            const float upper = (edge_extent - origin) / direction;
            enter = (std::max) (enter, (std::min) (lower, upper));
            exit = (std::min) (exit, (std::max) (lower, upper));
        }

        if (not (exit > enter)) {
            return std::nullopt;
        }

        // Select the cell entered at the clipped start: an exact boundary uses
        // the ray direction, while a parallel component keeps the upper cell.
        const auto entered_cell = [](const float position, const float direction) noexcept -> i32 {
            i32 cell = static_cast<i32>(std::floor(position));
            if (position == static_cast<float>(cell) and direction < 0.0f) {
                --cell;
            }

            return cell;
        };

        const float2 clipped{start.x + enter * delta.x, start.y + enter * delta.y};
        i32 cell_x = entered_cell(clipped.x, delta.x);
        i32 cell_y = entered_cell(clipped.y, delta.y);
        const auto cell_in_range = [edge](const i32 value) noexcept {
            return value >= 0 and value < static_cast<i32>(edge);
        };
        if (not cell_in_range(cell_x) or not cell_in_range(cell_y)) {
            return std::nullopt;
        }

        float previous = enter;
        for (;;) {
            const u32 cell = static_cast<u32>(cell_y) * edge + static_cast<u32>(cell_x);
            if (occupancy[cell] != 0u) {
                return cell;
            }

            // Next face parameters are rebuilt from the original ray so no
            // accumulated position nudge can drift the traversal.
            const float next_x = delta.x > 0.0f   ? (static_cast<float>(cell_x + 1) - start.x) / delta.x
                                 : delta.x < 0.0f ? (static_cast<float>(cell_x) - start.x) / delta.x
                                                  : std::numeric_limits<float>::infinity();
            const float next_y = delta.y > 0.0f   ? (static_cast<float>(cell_y + 1) - start.y) / delta.y
                                 : delta.y < 0.0f ? (static_cast<float>(cell_y) - start.y) / delta.y
                                                  : std::numeric_limits<float>::infinity();
            const float next = (std::min) (next_x, next_y);

            // A nonfinite or non-advancing crossing is a traversal failure, not
            // a clear interval.
            if (not std::isfinite(next) or next <= previous) {
                return std::nullopt;
            }

            // A crossing at the clipped end (including t=1) belongs to the
            // following segment and is excluded.
            if (next >= exit) {
                return std::nullopt;
            }

            if (next_x < next_y) {
                cell_x += delta.x > 0.0f ? 1 : -1;
            } else if (next_y < next_x) {
                cell_y += delta.y > 0.0f ? 1 : -1;
            } else {
                cell_x += delta.x > 0.0f ? 1 : -1;
                cell_y += delta.y > 0.0f ? 1 : -1;
            }

            if (not cell_in_range(cell_x) or not cell_in_range(cell_y)) {
                return std::nullopt;
            }

            previous = next;
        }
    }
}
