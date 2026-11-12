module;

#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <slang.h>

module engine.tests.app;

import std;
import engine.core;
import engine.app;
import engine.math;
import engine.shader;
import engine.rhi;

namespace pP::tests::detail::WorldLightingCodec {
    using lighting::CH;
    using lighting::Interval;
    using lighting::PackedCH;
    using lighting::PackedInterval;
    using lighting::SourceMaterial;
    using lighting::kThermalLutBytes;
    using lighting::kThermalLutMaxKelvin;
    using lighting::kThermalLutMinKelvin;
    using lighting::kThermalLutRows;
    using lighting::kThermalLutStride;
    using lighting::lastTraceFrame;
    using lighting::makeSourceMaterial;
    using lighting::packCell;
    using lighting::packCH;
    using lighting::packInterval;
    using lighting::packRGB9E5;
    using lighting::unpackCH;
    using lighting::unpackInterval;
    using lighting::unpackRGB9E5;

    // ------------------------------------------------------------------
    // frozen binary64 colour reference
    // ------------------------------------------------------------------
    // The same pinned 360-830 nm CIE 1931 2-degree rows the offline generator
    // consumes. This file is an independent transcription used only to check
    // the committed table; it is not a runtime asset.

    struct CieSample {
        double m_wavelength_nm;
        double m_x;
        double m_y;
        double m_z;
    };

    inline constexpr CieSample kCieSamples[] = {
#include "../../../../third_party/CIE/CIE_xyz_1931_2deg_reference.inc"
    };

    static_assert(std::size(kCieSamples) == 471u);
    static_assert(kCieSamples[0].m_wavelength_nm == 360.0 and kCieSamples[470].m_wavelength_nm == 830.0);

    inline constexpr double kPlanckH = 6.62607015e-34;
    inline constexpr double kPlanckC = 299792458.0;
    inline constexpr double kBoltzmann = 1.380649e-23;
    inline constexpr double kNanometre = 1e-9;
    inline constexpr double kExponentCutoff = 700.0;
    inline constexpr double kCalibrationKelvin = 6500.0;

    struct RgbReference {
        double m_r, m_g, m_b;
    };

    [[nodiscard]] double planckRadiance(const double wavelength_m, const double kelvin) noexcept {
        if (kelvin <= 0.0) {
            return 0.0;
        }

        const double exponent = kPlanckH * kPlanckC / (wavelength_m * kBoltzmann * kelvin);
        if (exponent > kExponentCutoff) {
            return 0.0;
        }

        return 2.0 * kPlanckH * kPlanckC * kPlanckC / (std::pow(wavelength_m, 5.0) * std::expm1(exponent));
    }

    [[nodiscard]] std::array<double, 3u> integrateXyz(const double kelvin) noexcept {
        std::array<double, 3u> xyz{};
        if (kelvin <= 0.0) {
            return xyz;
        }

        for (std::size_t index = 0u; index < std::size(kCieSamples); ++index) {
            const CieSample &sample = kCieSamples[index];
            const double weight = index == 0u or index + 1u == std::size(kCieSamples) ? 0.5 : 1.0;
            const double scale = planckRadiance(sample.m_wavelength_nm * kNanometre, kelvin) * weight * kNanometre;
            xyz[0] += scale * sample.m_x;
            xyz[1] += scale * sample.m_y;
            xyz[2] += scale * sample.m_z;
        }

        return xyz;
    }

    [[nodiscard]] RgbReference linearSrgb(const std::array<double, 3u> &xyz) noexcept {
        if (xyz[0] == 0.0 and xyz[1] == 0.0 and xyz[2] == 0.0) {
            return {};
        }

        std::array<double, 3u> linear{
            (std::max) (3.2406 * xyz[0] - 1.5372 * xyz[1] - 0.4986 * xyz[2], 0.0),
            (std::max) (-0.9689 * xyz[0] + 1.8758 * xyz[1] + 0.0415 * xyz[2], 0.0),
            (std::max) (0.0557 * xyz[0] - 0.2040 * xyz[1] + 1.0570 * xyz[2], 0.0)};

        const double luminance = 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2];
        if (luminance <= 0.0) {
            return {};
        }

        const double scale = xyz[1] / luminance;
        return {linear[0] * scale, linear[1] * scale, linear[2] * scale};
    }

    [[nodiscard]] double calibrationY() noexcept {
        static const double reference = integrateXyz(kCalibrationKelvin)[1];
        return reference;
    }

    [[nodiscard]] RgbReference engineReference(const double kelvin) noexcept {
        if (kelvin <= 0.0) {
            return {};
        }

        const RgbReference rgb = linearSrgb(integrateXyz(kelvin));
        const double reference = calibrationY();
        return {rgb.m_r / reference, rgb.m_g / reference, rgb.m_b / reference};
    }

    [[nodiscard]] double referenceLuminance(const RgbReference &rgb) noexcept {
        return 0.2126 * rgb.m_r + 0.7152 * rgb.m_g + 0.0722 * rgb.m_b;
    }

    // ------------------------------------------------------------------
    // CPU codec values
    // ------------------------------------------------------------------

    [[nodiscard]] Interval makeInterval(
        const float red, const float green, const float blue, const float transmittance) {
        Interval value{};
        value.m_radiance = float3{red, green, blue};
        value.m_transmittance = transmittance;
        return value;
    }

    [[nodiscard]] CH zeroCH() {
        CH value{};
        for (float3 &coefficient: value.m_coefficients) {
            coefficient = float3{zero_v};
        }
        return value;
    }

    [[nodiscard]] u16 intervalHalf(const PackedInterval &value, const u32 word, const bool high) noexcept {
        const u32 raw = word == 0u ? value.m_rg : value.m_bt;
        return static_cast<u16>((raw >> (high ? 16u : 0u)) & 0xffffu);
    }

    [[nodiscard]] float halfValue(const u16 bits) noexcept {
        return unpackInterval(PackedInterval{static_cast<u32>(bits), 0u}).m_radiance.x;
    }

    // ------------------------------------------------------------------
    // codec contracts
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (record_sizes_and_exact_zero_encoding) {
        static_assert(sizeof(PackedInterval) == 8u, "coarse interval stride");
        static_assert(sizeof(PackedCH) == 32u, "fine CH stride");
        static_assert(sizeof(SourceMaterial) == 8u, "source material stride");
        static_assert(PPR_OFFSETOF(PackedInterval, m_bt) == 4u);
        static_assert(PPR_OFFSETOF(SourceMaterial, m_diffuse) == 4u);

        PPR_TEST_ASSERT(packCell(0u, 1234.5) == 0u);
        // Empty cells carry no meaningful temperature: material zero is the
        // canonical all-zero word even if a stale temperature is supplied.
        PPR_TEST_ASSERT(packCell(0u, -1.0) == 0u);

        const auto zero_rgb = packRGB9E5(float3{zero_v});
        PPR_TEST_ASSERT(zero_rgb.has_value() and *zero_rgb == 0u);

        const auto zero_interval = packInterval(makeInterval(0.0f, 0.0f, 0.0f, 0.0f));
        PPR_TEST_ASSERT(zero_interval.has_value() and zero_interval->m_rg == 0u and zero_interval->m_bt == 0u);

        const auto zero_ch = packCH(zeroCH());
        PPR_TEST_ASSERT(zero_ch.has_value());
        for (const u32 word: zero_ch->m_words) {
            PPR_TEST_ASSERT(word == 0u);
        }

        const auto zero_material = makeSourceMaterial(
            float3{zero_v}, 0.0, float3{zero_v}, float3{1.0f, 1.0f, 1.0f}, 0.0f);
        PPR_TEST_ASSERT(
            zero_material.has_value() and zero_material->m_emission == 0u and zero_material->m_diffuse == 0u);

        const auto decoded_interval = unpackInterval(PackedInterval{});
        PPR_TEST_ASSERT(
            decoded_interval.m_radiance.x == 0.0f and
            decoded_interval.m_radiance.y == 0.0f and
            decoded_interval.m_radiance.z == 0.0f and
            decoded_interval.m_transmittance == 0.0f);
        PPR_TEST_ASSERT(unpackRGB9E5(0u).x == 0.0f and unpackRGB9E5(0u).y == 0.0f and unpackRGB9E5(0u).z == 0.0f);
    };

    // ------------------------------------------------------------------
    // binary16 rounding and interval range
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (interval_half_rounding_ties_subnormals_and_maximum) {
        const auto encode = [](const float value) -> Expected<PackedInterval> {
            return packInterval(makeInterval(value, 0.0f, 0.0f, 1.0f));
        };

        // Nearest/ties-to-even: both exact midpoints select the even neighbour.
        const auto tie_low = encode(1.00048828125f);
        PPR_TEST_ASSERT(tie_low.has_value() and intervalHalf(*tie_low, 0u, false) == 0x3c00u);
        const auto tie_high = encode(1.00146484375f);
        PPR_TEST_ASSERT(tie_high.has_value() and intervalHalf(*tie_high, 0u, false) == 0x3c02u);

        const auto min_subnormal = encode(std::ldexp(1.0f, -24));
        PPR_TEST_ASSERT(min_subnormal.has_value() and intervalHalf(*min_subnormal, 0u, false) == 0x0001u);
        PPR_TEST_ASSERT(halfValue(0x0001u) == std::ldexp(1.0f, -24));

        const auto half_min_subnormal = encode(std::ldexp(1.0f, -25));
        PPR_TEST_ASSERT(half_min_subnormal.has_value() and intervalHalf(*half_min_subnormal, 0u, false) == 0x0000u);
        const auto three_halves = encode(std::ldexp(3.0f, -25));
        PPR_TEST_ASSERT(three_halves.has_value() and intervalHalf(*three_halves, 0u, false) == 0x0002u);
        PPR_TEST_ASSERT(halfValue(0x0002u) == std::ldexp(1.0f, -23));

        const auto smallest_normal = encode(std::ldexp(1.0f, -14));
        PPR_TEST_ASSERT(smallest_normal.has_value() and intervalHalf(*smallest_normal, 0u, false) == 0x0400u);
        const auto maximum = encode(65504.0f);
        PPR_TEST_ASSERT(maximum.has_value() and intervalHalf(*maximum, 0u, false) == 0x7bffu);
        PPR_TEST_ASSERT(halfValue(0x7bffu) == 65504.0f);

        const auto over_maximum = encode(65504.5f);
        PPR_TEST_ASSERT(not over_maximum.has_value() and over_maximum.error() == std::errc::result_out_of_range);

        const auto negative = encode(-1.0f);
        PPR_TEST_ASSERT(not negative.has_value() and negative.error() == std::errc::invalid_argument);
        const auto not_a_number = encode(std::numeric_limits<float>::quiet_NaN());
        PPR_TEST_ASSERT(not not_a_number.has_value() and not_a_number.error() == std::errc::invalid_argument);
        const auto infinity = encode(std::numeric_limits<float>::infinity());
        PPR_TEST_ASSERT(not infinity.has_value() and infinity.error() == std::errc::invalid_argument);

        const auto bad_transmittance = packInterval(makeInterval(1.0f, 1.0f, 1.0f, 1.5f));
        PPR_TEST_ASSERT(not bad_transmittance.has_value());
        const auto nan_transmittance = packInterval(
            makeInterval(1.0f, 1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()));
        PPR_TEST_ASSERT(not nan_transmittance.has_value());

        _.log(
            std::format(
                "WORLD_LIGHTING_CODEC interval_half=0x{:04x},0x{:04x},0x{:04x} max=0x{:04x}",
                intervalHalf(*tie_low, 0u, false), intervalHalf(*tie_high, 0u, false),
                intervalHalf(*min_subnormal, 0u, false), intervalHalf(*maximum, 0u, false)));
    };

    // ------------------------------------------------------------------
    // RGB9E5 rounding, exponent selection and range
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (rgb9e5_exponent_boundaries_ties_and_range) {
        PPR_TEST_ASSERT(*packRGB9E5(float3{zero_v}) == 0u);

        const auto one = packRGB9E5(float3{1.0f, 1.0f, 1.0f});
        PPR_TEST_ASSERT(one.has_value() and ((*one >> 27u) & 0x1fu) == 16u);
        PPR_TEST_ASSERT(
            (*one & 0x1ffu) == 256u and ((*one >> 9u) & 0x1ffu) == 256u and ((*one >> 18u) & 0x1ffu) == 256u);
        PPR_TEST_ASSERT(unpackRGB9E5(*one).x == 1.0f and unpackRGB9E5(*one).y == 1.0f and unpackRGB9E5(*one).z == 1.0f);

        const auto maximum = packRGB9E5(float3{65408.0f, 65408.0f, 65408.0f});
        PPR_TEST_ASSERT(maximum.has_value() and ((*maximum >> 27u) & 0x1fu) == 31u);
        PPR_TEST_ASSERT((*maximum & 0x1ffu) == 511u);
        PPR_TEST_ASSERT(
            unpackRGB9E5(*maximum).x == 65408.0f and
            unpackRGB9E5(*maximum).y == 65408.0f and
            unpackRGB9E5(*maximum).z == 65408.0f);

        const auto over_maximum = packRGB9E5(float3{65408.5f, 0.0f, 0.0f});
        PPR_TEST_ASSERT(not over_maximum.has_value() and over_maximum.error() == std::errc::result_out_of_range);

        // Ties round up for the unsigned shared-exponent codec.
        const auto tie_up = packRGB9E5(float3{std::ldexp(1.0f, -25), 0.0f, 0.0f});
        PPR_TEST_ASSERT(tie_up.has_value() and *tie_up == 1u);
        PPR_TEST_ASSERT(unpackRGB9E5(*tie_up).x == std::ldexp(1.0f, -24));
        const auto tie_two = packRGB9E5(float3{3.0f * std::ldexp(1.0f, -25), 0.0f, 0.0f});
        PPR_TEST_ASSERT(tie_two.has_value() and (*tie_two & 0x1ffu) == 2u);

        // A tiny positive value legitimately quantizes to zero.
        PPR_TEST_ASSERT(*packRGB9E5(float3{1.0e-9f, 0.0f, 0.0f}) == 0u);

        const auto five_eleven = packRGB9E5(float3{511.0f, 0.0f, 0.0f});
        PPR_TEST_ASSERT(five_eleven.has_value() and ((*five_eleven >> 27u) & 0x1fu) == 24u);
        PPR_TEST_ASSERT(unpackRGB9E5(*five_eleven).x == 511.0f);

        const auto negative = packRGB9E5(float3{-1.0f, 0.0f, 0.0f});
        PPR_TEST_ASSERT(not negative.has_value() and negative.error() == std::errc::invalid_argument);
        const auto not_a_number = packRGB9E5(float3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f});
        PPR_TEST_ASSERT(not not_a_number.has_value() and not_a_number.error() == std::errc::invalid_argument);
        const auto infinity = packRGB9E5(float3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f});
        PPR_TEST_ASSERT(not infinity.has_value() and infinity.error() == std::errc::invalid_argument);

        _.log(
            std::format("WORLD_LIGHTING_CODEC rgb9e5_one=0x{:08x} max=0x{:08x} tie=0x{:08x}", *one, *maximum, *tie_up));
    };

    // ------------------------------------------------------------------
    // signed AC endpoints, zero scale and DC range
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (ch_signed_ac_endpoints_scale_zero_and_dc_range) {
        CH positive = zeroCH();
        positive.m_coefficients[0] = float3{1.0f, 1.0f, 1.0f};
        positive.m_coefficients[1] = float3{127.0f, 0.0f, 0.0f};
        const auto packed_positive = packCH(positive);
        PPR_TEST_ASSERT(packed_positive.has_value());
        // max(abs(AC)) is 127, so scale is 1.0 and the endpoint byte is +127.
        PPR_TEST_ASSERT(std::bit_cast<float>(packed_positive->m_words[1]) == 1.0f);
        PPR_TEST_ASSERT(((packed_positive->m_words[2] >> 0u) & 0xffu) == 127u);
        const CH decoded_positive = unpackCH(*packed_positive);
        PPR_TEST_ASSERT(decoded_positive.m_coefficients[0].x == 1.0f);
        PPR_TEST_ASSERT(decoded_positive.m_coefficients[1].x == 127.0f);
        PPR_TEST_ASSERT(decoded_positive.m_coefficients[1].y == 0.0f);

        CH negative = zeroCH();
        negative.m_coefficients[1] = float3{-127.0f, 0.0f, 0.0f};
        const auto packed_negative = packCH(negative);
        PPR_TEST_ASSERT(packed_negative.has_value());
        PPR_TEST_ASSERT(((packed_negative->m_words[2] >> 0u) & 0xffu) == 0x81u);
        PPR_TEST_ASSERT(((packed_negative->m_words[2] >> 0u) & 0xffu) != 0x80u);
        PPR_TEST_ASSERT(unpackCH(*packed_negative).m_coefficients[1].x == -127.0f);

        for (u32 index = 2u; index < packed_negative->m_words.size(); ++index) {
            const u32 word = packed_negative->m_words[index];
            for (u32 byte = 0u; byte < 4u; ++byte) {
                PPR_TEST_ASSERT(((word >> (byte * 8u)) & 0xffu) != 0x80u);
            }
        }

        const auto zero_ac = packCH(zeroCH());
        PPR_TEST_ASSERT(zero_ac.has_value());
        PPR_TEST_ASSERT(zero_ac->m_words[0] == 0u and zero_ac->m_words[1] == 0u);
        for (u32 index = 2u; index < zero_ac->m_words.size(); ++index) {
            PPR_TEST_ASSERT(zero_ac->m_words[index] == 0u);
        }

        // Nonzero AC whose FP32 scale underflows encodes zero AC, not an error.
        CH underflow = zeroCH();
        underflow.m_coefficients[1] = float3{std::numeric_limits<float>::denorm_min(), 0.0f, 0.0f};
        const auto packed_underflow = packCH(underflow);
        PPR_TEST_ASSERT(packed_underflow.has_value());
        PPR_TEST_ASSERT(packed_underflow->m_words[1] == 0u);
        for (u32 index = 2u; index < packed_underflow->m_words.size(); ++index) {
            PPR_TEST_ASSERT(packed_underflow->m_words[index] == 0u);
        }

        CH out_of_range = zeroCH();
        out_of_range.m_coefficients[0] = float3{65409.0f, 0.0f, 0.0f};
        const auto packed_out_of_range = packCH(out_of_range);
        PPR_TEST_ASSERT(
            not packed_out_of_range.has_value() and packed_out_of_range.error() == std::errc::result_out_of_range);

        CH not_a_number = zeroCH();
        not_a_number.m_coefficients[3] = float3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
        const auto packed_not_a_number = packCH(not_a_number);
        PPR_TEST_ASSERT(
            not packed_not_a_number.has_value() and packed_not_a_number.error() == std::errc::invalid_argument);

        // Mixed signs and magnitudes round-trip through the shared scale.
        CH mixed = zeroCH();
        mixed.m_coefficients[1] = float3{2.0f, -1.0f, 0.25f};
        mixed.m_coefficients[5] = float3{-0.5f, 0.125f, 0.0f};
        const auto packed_mixed = packCH(mixed);
        PPR_TEST_ASSERT(packed_mixed.has_value());
        const CH decoded_mixed = unpackCH(*packed_mixed);
        for (u32 coefficient = 1u; coefficient < 9u; ++coefficient) {
            const float3 expected = mixed.m_coefficients[coefficient];
            const float3 actual = decoded_mixed.m_coefficients[coefficient];
            const float tolerance = std::abs(std::bit_cast<float>(packed_mixed->m_words[1])) * 0.5f;
            PPR_TEST_ASSERT(std::abs(expected.x - actual.x) <= tolerance + 1.0e-6f);
            PPR_TEST_ASSERT(std::abs(expected.y - actual.y) <= tolerance + 1.0e-6f);
            PPR_TEST_ASSERT(std::abs(expected.z - actual.z) <= tolerance + 1.0e-6f);
        }

        _.log(
            std::format(
                "WORLD_LIGHTING_CODEC ch_endpoint_positive={} ch_endpoint_negative={} ch_zero_scale=true",
                decoded_positive.m_coefficients[1].x, unpackCH(*packed_negative).m_coefficients[1].x));
    };

    // ------------------------------------------------------------------
    // resolved-cell packing
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (cell_rounds_to_binary16_and_rejects_invalid_kelvin) {
        const auto zero = packCell(7u, 0.0);
        PPR_TEST_ASSERT(zero.has_value() and (*zero & 0xffffu) == 7u and (*zero >> 16u) == 0u);
        const auto negative_zero = packCell(7u, -0.0);
        PPR_TEST_ASSERT(negative_zero.has_value() and *negative_zero == *zero);

        const auto tie_down = packCell(7u, 1234.5);
        PPR_TEST_ASSERT(tie_down.has_value() and halfValue(static_cast<u16>(*tie_down >> 16u)) == 1234.0f);
        const auto tie_up = packCell(7u, 1235.5);
        PPR_TEST_ASSERT(tie_up.has_value() and halfValue(static_cast<u16>(*tie_up >> 16u)) == 1236.0f);

        const auto maximum = packCell(3u, 65504.0);
        PPR_TEST_ASSERT(maximum.has_value() and (*maximum >> 16u) == 0x7bffu);
        PPR_TEST_ASSERT(halfValue(static_cast<u16>(*maximum >> 16u)) == 65504.0f);
        PPR_TEST_ASSERT((*maximum & 0xffffu) == 3u);

        const auto reserved_material = packCell(0xffffu, 300.0);
        PPR_TEST_ASSERT(reserved_material.has_value() and (*reserved_material & 0xffffu) == 0xffffu);

        const auto over_maximum = packCell(1u, 65504.5);
        PPR_TEST_ASSERT(not over_maximum.has_value() and over_maximum.error() == std::errc::result_out_of_range);
        const auto negative = packCell(1u, -1.0);
        PPR_TEST_ASSERT(not negative.has_value() and negative.error() == std::errc::invalid_argument);
        const auto not_a_number = packCell(1u, std::numeric_limits<double>::quiet_NaN());
        PPR_TEST_ASSERT(not not_a_number.has_value() and not_a_number.error() == std::errc::invalid_argument);
        const auto infinity = packCell(1u, std::numeric_limits<double>::infinity());
        PPR_TEST_ASSERT(not infinity.has_value() and infinity.error() == std::errc::invalid_argument);

        _.log(std::format("WORLD_LIGHTING_CODEC cell_zero=0x{:08x} cell_max=0x{:08x}", *zero, *maximum));
    };

    // ------------------------------------------------------------------
    // eight-byte source material
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (source_material_multiplies_emission_in_binary64_and_zeroes_metals) {
        const auto encode = [](const double red, const double green, const double blue) -> u32 {
            const Expected<u32> word = packRGB9E5(
                float3{static_cast<float>(red), static_cast<float>(green), static_cast<float>(blue)});
            PPR_TEST_ASSERT(word.has_value());
            return *word;
        };

        const float3 emission{0.4f, 0.5f, 0.6f};
        const double strength = 3.0;
        const float3 tint{0.5f, 0.25f, 1.0f};
        const float3 mean{0.8f, 0.6f, 0.4f};
        const float metallic = 0.25f;

        const auto material = makeSourceMaterial(emission, strength, tint, mean, metallic);
        PPR_TEST_ASSERT(material.has_value());
        PPR_TEST_ASSERT(
            material->m_emission == encode(
                                        static_cast<double>(emission.x) * strength,
                                        static_cast<double>(emission.y) * strength,
                                        static_cast<double>(emission.z) * strength));
        PPR_TEST_ASSERT(
            material->m_diffuse ==
            encode(
                static_cast<double>(tint.x) * static_cast<double>(mean.x) * (1.0 - static_cast<double>(metallic)),
                static_cast<double>(tint.y) * static_cast<double>(mean.y) * (1.0 - static_cast<double>(metallic)),
                static_cast<double>(tint.z) * static_cast<double>(mean.z) * (1.0 - static_cast<double>(metallic))));

        const auto pure_metal = makeSourceMaterial(emission, strength, tint, mean, 1.0f);
        PPR_TEST_ASSERT(pure_metal.has_value() and pure_metal->m_diffuse == 0u);

        // A missing albedo texture contributes white.
        const auto white_mean = makeSourceMaterial(float3{zero_v}, 0.0, tint, float3{1.0f, 1.0f, 1.0f}, 0.0f);
        PPR_TEST_ASSERT(
            white_mean.has_value() and
            white_mean->m_diffuse ==
                encode(static_cast<double>(tint.x), static_cast<double>(tint.y), static_cast<double>(tint.z)));

        const auto zero_tint = makeSourceMaterial(float3{zero_v}, 0.0, float3{zero_v}, mean, 0.0f);
        PPR_TEST_ASSERT(zero_tint.has_value() and zero_tint->m_diffuse == 0u);

        // The binary64 product is admitted at exactly 65,408 and rejected above.
        const auto admissible = makeSourceMaterial(
            float3{1.0f, 0.0f, 0.0f}, 65408.0, float3{zero_v}, float3{1.0f, 1.0f, 1.0f}, 0.0f);
        PPR_TEST_ASSERT(admissible.has_value() and unpackRGB9E5(admissible->m_emission).x == 65408.0f);
        const auto overflow = makeSourceMaterial(
            float3{1.0f, 0.0f, 0.0f}, 65504.0, float3{zero_v}, float3{1.0f, 1.0f, 1.0f}, 0.0f);
        PPR_TEST_ASSERT(not overflow.has_value() and overflow.error() == std::errc::result_out_of_range);
        const auto huge = makeSourceMaterial(
            float3{1.0f, 0.0f, 0.0f}, 1.0e308, float3{zero_v}, float3{1.0f, 1.0f, 1.0f}, 0.0f);
        PPR_TEST_ASSERT(not huge.has_value() and huge.error() == std::errc::result_out_of_range);

        const auto invalid = [](const Expected<SourceMaterial> &result) {
            return not result.has_value() and result.error() == std::errc::invalid_argument;
        };
        PPR_TEST_ASSERT(invalid(makeSourceMaterial(float3{-0.1f, 0.0f, 0.0f}, 1.0, tint, mean, 0.0f)));
        PPR_TEST_ASSERT(invalid(
            makeSourceMaterial(float3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, 1.0, tint, mean, 0.0f)));
        PPR_TEST_ASSERT(invalid(makeSourceMaterial(emission, -1.0, tint, mean, 0.0f)));
        PPR_TEST_ASSERT(
            invalid(makeSourceMaterial(emission, std::numeric_limits<double>::quiet_NaN(), tint, mean, 0.0f)));
        PPR_TEST_ASSERT(invalid(makeSourceMaterial(emission, strength, float3{1.1f, 0.0f, 0.0f}, mean, 0.0f)));
        PPR_TEST_ASSERT(invalid(makeSourceMaterial(emission, strength, tint, float3{-0.1f, 0.0f, 0.0f}, 0.0f)));
        PPR_TEST_ASSERT(invalid(makeSourceMaterial(emission, strength, tint, mean, 1.1f)));
        PPR_TEST_ASSERT(
            invalid(makeSourceMaterial(emission, strength, tint, mean, std::numeric_limits<float>::quiet_NaN())));

        _.log(
            std::format(
                "WORLD_LIGHTING_CODEC source_emission=0x{:08x} source_diffuse=0x{:08x} metal_diffuse=0x{:08x}",
                material->m_emission, material->m_diffuse, pure_metal->m_diffuse));
    };

    // ------------------------------------------------------------------
    // retained direction identity
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (last_trace_frame_follows_the_four_frame_schedule) {
        PPR_TEST_ASSERT(lastTraceFrame(0u, 0u, 0u) == 0u);
        PPR_TEST_ASSERT(not lastTraceFrame(0u, 0u, 1u).has_value());
        PPR_TEST_ASSERT(lastTraceFrame(1u, 0u, 1u) == 1u);
        PPR_TEST_ASSERT(lastTraceFrame(3u, 0u, 3u) == 3u);
        PPR_TEST_ASSERT(lastTraceFrame(4u, 0u, 3u) == 3u);
        PPR_TEST_ASSERT(lastTraceFrame(5u, 0u, 3u) == 3u);
        PPR_TEST_ASSERT(not lastTraceFrame(2u, 0u, 3u).has_value());
        PPR_TEST_ASSERT(lastTraceFrame(4u, 1u, 0u) == 3u);
        PPR_TEST_ASSERT(lastTraceFrame(7u, 1u, 0u) == 7u);
        PPR_TEST_ASSERT(lastTraceFrame(3u, 3u, 2u) == 3u);

        PPR_TEST_ASSERT(not lastTraceFrame(4u, 4u, 0u).has_value());
        PPR_TEST_ASSERT(not lastTraceFrame(4u, 0u, 4u).has_value());

        constexpr u64 maximum = std::numeric_limits<u64>::max();
        PPR_TEST_ASSERT(lastTraceFrame(maximum, 0u, 0u) == maximum - 3u);

        for (u64 frame = 0u; frame < 32u; ++frame) {
            for (u32 phase = 0u; phase < 4u; ++phase) {
                for (u32 probe = 0u; probe < 4u; ++probe) {
                    const u32 age = (static_cast<u32>(frame & 3u) + phase + 4u - probe) & 3u;
                    const auto trace = lastTraceFrame(frame, phase, probe);
                    if (frame >= age) {
                        PPR_TEST_ASSERT(trace.has_value() and *trace == frame - age);
                        PPR_TEST_ASSERT((*trace + phase) % 4u == probe);
                    } else {
                        PPR_TEST_ASSERT(not trace.has_value());
                    }
                }
            }
        }

        _.log(std::format("WORLD_LIGHTING_CODEC last_trace_frame_schedule=fixed4"));
    };

    // ------------------------------------------------------------------
    // device fixture and codec probe plumbing
    // ------------------------------------------------------------------

    inline constexpr u32 kRecordCount = 13u;

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue;

        void initialize(UnitTest::IRun &_) {
            PPR_TEST_ASSERT(not m_shaders->initialize());
#ifdef _WIN32
            constexpr auto backend = rhi::DeviceType::D3D12;
#else
            constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
            PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
            PPR_TEST_ASSERT(m_rhi->getDevice().getDeviceType() == backend);
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            const rhi::DeviceInfo &info = m_rhi->getDevice().getInfo();
            _.log(
                std::format(
                    "WORLD_LIGHTING_CODEC api={} adapter={} backend={}",
                    info.apiName != nullptr ? info.apiName : "unknown",
                    info.adapterName != nullptr ? info.adapterName : "unknown", static_cast<int>(info.deviceType)));
        }

        void shutdown() {
            m_queue.setNull();
            PPR_TEST_ASSERT(not m_rhi->shutdown());
            PPR_TEST_ASSERT(not m_shaders->shutdown());
        }

        [[nodiscard]] rhi::IDevice &device() {
            return m_rhi->getDevice();
        }
    };

    [[nodiscard]] rhi::ComPtr<rhi::IShaderProgram> makeProgram(
        rhi::IDevice &device, shader::SharedModule &module, const std::span<const char *const> names) {
        std::array<shader::ComPtr<shader::IEntryPoint>, 2> entries{};
        std::array<shader::IComponentType *, 2> components{};
        PPR_TEST_ASSERT(names.size() <= entries.size());

        for (u32 index = 0u; index < names.size(); ++index) {
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(names[index], entries[index].writeRef())));
            components[index] = entries[index].get();
        }

        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = module.get();
        description.slangEntryPoints = components.data();
        description.slangEntryPointCount = static_cast<u32>(names.size());

        rhi::ComPtr<rhi::IShaderProgram> program;
        shader::Diagnose diagnostics;
        PPR_TEST_ASSERT(
            not make_error_code(device.createShaderProgram(description, program.writeRef(), diagnostics.writeRef())));
        return program;
    }

    struct ComputePipeline {
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline;
        rhi::ComPtr<rhi::IShaderObject> m_root;
    };

    [[nodiscard]] ComputePipeline makeComputePipeline(rhi::IDevice &device, rhi::IShaderProgram *const program) {
        ComputePipeline result{};
        rhi::ComputePipelineDesc description{};
        description.program = program;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(description, result.m_pipeline.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, result.m_root.writeRef())));
        return result;
    }

    void bindBuffer(rhi::IShaderObject &root, const char *const name, rhi::IBuffer *const buffer) {
        PPR_TEST_ASSERT(not make_error_code(rhi::ShaderCursor{&root}[name].setBinding(rhi::Binding(buffer))));
    }

    [[nodiscard]] rhi::ComPtr<rhi::ICommandEncoder> makeEncoder(Fixture &fixture) {
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        return encoder;
    }

    void submit(Fixture &fixture, rhi::ICommandEncoder &encoder) {
        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));

        bool submitted = true;
        PPR_DEFER {
            if (submitted) {
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
            }
        };

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        submitted = false;
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeStorageBuffer(
        rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial = nullptr) {
        rhi::BufferDesc description{};
        description.size = bytes;
        description.elementSize = stride;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::ShaderResource) |
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
            static_cast<u32>(rhi::BufferUsage::CopySource) |
            static_cast<u32>(rhi::BufferUsage::CopyDestination));
        description.defaultState = rhi::ResourceState::UnorderedAccess;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, initial, buffer.writeRef())));
        return buffer;
    }

    [[nodiscard]] slang::TypeLayoutReflection *parameterElementLayout(
        slang::ProgramLayout *const layout, const std::string_view name) {
        PPR_TEST_ASSERT(layout != nullptr);
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (parameter->getName() != nullptr and name == std::string_view{parameter->getName()}) {
                return parameter->getTypeLayout()->getElementTypeLayout();
            }
        }

        return nullptr;
    }

    [[nodiscard]] u32 reflectedFieldOffset(slang::TypeLayoutReflection *const type, const std::string_view name) {
        for (u32 index = 0u; index < type->getFieldCount(); ++index) {
            auto *const field = type->getFieldByIndex(index);
            if (field->getName() != nullptr and name == std::string_view{field->getName()}) {
                return static_cast<u32>(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM));
            }
        }

        return 0xffff'ffffu;
    }

    struct CodecProbeParameters {
        u32 m_count{};
        u32 m_reserved[3]{};
    };

    static_assert(sizeof(CodecProbeParameters) == 16u);

    void setCodecProbe(rhi::IShaderObject &root, const u32 count) {
        const CodecProbeParameters parameters{.m_count = count};
        rhi::ShaderCursor cursor{&root};
        rhi::ShaderCursor constants{};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_codec_probe"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
    }

    // ------------------------------------------------------------------
    // reflected record ABI
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (gpu_reflection_matches_record_strides_and_word_offsets) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        auto &device = fixture.device();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_codec_probe.slang",
            "world_lighting_codec_probe", module.writeRef()));

        auto *const layout = module->getLayout();
        auto *const intervals = parameterElementLayout(layout, "g_codec_intervals");
        auto *const fine_ch = parameterElementLayout(layout, "g_codec_fine_ch");
        auto *const sources = parameterElementLayout(layout, "g_codec_source_materials");
        PPR_TEST_ASSERT(intervals != nullptr and fine_ch != nullptr and sources != nullptr);

        PPR_TEST_ASSERT(intervals->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u);
        PPR_TEST_ASSERT(intervals->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u);
        PPR_TEST_ASSERT(reflectedFieldOffset(intervals, "m_rg") == 0u);
        PPR_TEST_ASSERT(reflectedFieldOffset(intervals, "m_bt") == 4u);

        PPR_TEST_ASSERT(fine_ch->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u);
        PPR_TEST_ASSERT(fine_ch->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u);
        PPR_TEST_ASSERT(fine_ch->getFieldCount() == 3u);
        PPR_TEST_ASSERT(reflectedFieldOffset(fine_ch, "m_dc") == 0u);
        PPR_TEST_ASSERT(reflectedFieldOffset(fine_ch, "m_scale") == 4u);
        PPR_TEST_ASSERT(reflectedFieldOffset(fine_ch, "m_ac") == 8u);

        PPR_TEST_ASSERT(sources->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u);
        PPR_TEST_ASSERT(sources->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u);
        PPR_TEST_ASSERT(reflectedFieldOffset(sources, "m_emission") == 0u);
        PPR_TEST_ASSERT(reflectedFieldOffset(sources, "m_diffuse") == 4u);

        // Reflection alone is not execution: build a real compute pipeline
        // from the same module while the layout is qualified.
        constexpr const char *const entries[] = {"decodeRecords"};
        const auto program = makeProgram(device, module, entries);
        auto pipeline = makeComputePipeline(device, program);
        PPR_TEST_ASSERT(pipeline.m_pipeline.get() != nullptr and pipeline.m_root.get() != nullptr);

        _.log(std::format("WORLD_LIGHTING_CODEC reflection_strides=8/32/8 word_offsets=0/4/8..28"));
    };

    // ------------------------------------------------------------------
    // CPU and GPU decoded agreement
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (gpu_decodes_every_record_identically_to_the_cpu_codec) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        auto &device = fixture.device();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_codec_probe.slang",
            "world_lighting_codec_probe", module.writeRef()));

        constexpr const char *const entries[] = {"decodeRecords"};
        const auto program = makeProgram(device, module, entries);
        auto pipeline = makeComputePipeline(device, program);

        Array<PackedInterval> intervals(kRecordCount);
        constexpr std::array<float, kRecordCount> reds{
            0.0f, 0x1p-24f, 0x1p-14f, 1.0f, 1.00048828125f, 2.5f, 100.0f, 1234.5f, 32768.0f, 65504.0f, 0.25f, 65503.0f,
            0.0f};
        constexpr std::array<float, kRecordCount> greens{
            0.0f, 1.0f, 0.0f, 3.0f, 0.75f, 65504.0f, 42.0f, 0.0f, 1024.0f, 0.0f, 0.125f, 1.0f, 0.0f};
        constexpr std::array<float, kRecordCount> blues{
            0.0f, 0.5f, 0.25f, 7.0f, 1.0f, 0.0f, 0.5f, 2048.0f, 0.0f, 2.0f, 0.25f, 4.0f, 0.0f};
        constexpr std::array<float, kRecordCount> transmittance{
            0.0f, 1.0f, 0.5f, 0.25f, 0.75f, 0.0f, 1.0f, 0.125f, 0.875f, 0.5f, 1.0f, 0.0f, 0.5f};
        for (u32 index = 0u; index < kRecordCount; ++index) {
            const auto packed = packInterval(
                makeInterval(reds[index], greens[index], blues[index], transmittance[index]));
            PPR_TEST_ASSERT(packed.has_value());
            intervals[index] = *packed;
        }

        Array<PackedCH> fine_ch(kRecordCount);
        Array<CH> decoded_ch(kRecordCount);
        for (u32 index = 0u; index < kRecordCount; ++index) {
            CH value = zeroCH();
            const float dc = index == 0u ? 0.0f : index == kRecordCount - 1u ? 65408.0f : float(index) * 0.5f;
            value.m_coefficients[0] = float3{dc, dc * 0.5f, dc * 0.25f};
            if (index > 0u) {
                value.m_coefficients[1] = float3{float(index) * 1.5f, -float(index), 0.25f};
                value.m_coefficients[4] = float3{-0.5f, 0.125f, float(index) * 0.75f};
                value.m_coefficients[8] = float3{1.0f, -2.0f, 3.0f};
            }
            const auto packed = packCH(value);
            PPR_TEST_ASSERT(packed.has_value());
            fine_ch[index] = *packed;
            decoded_ch[index] = unpackCH(*packed);
        }

        Array<SourceMaterial> sources(kRecordCount);
        for (u32 index = 0u; index < kRecordCount; ++index) {
            const float metallic = index % 3u == 0u ? 1.0f : 0.25f;
            const auto material = makeSourceMaterial(
                float3{0.05f * float(index), 0.2f, 0.05f}, static_cast<double>(index) * 0.5, float3{0.5f, 0.4f, 0.3f},
                float3{0.9f, 0.8f, 0.7f}, metallic);
            PPR_TEST_ASSERT(material.has_value());
            sources[index] = *material;
        }

        Array<u32> cells(kRecordCount);
        constexpr std::array<double, kRecordCount> temperatures{
            0.0, 0.5, 256.0, 300.5, 1234.5, 4095.0, 6500.0, 10000.0, 65504.0, 65503.0, 1.0, 2.0, 3.0};
        for (u32 index = 0u; index < kRecordCount; ++index) {
            const auto cell = packCell(static_cast<u16>(index % 6u + 1u), temperatures[index]);
            PPR_TEST_ASSERT(cell.has_value());
            cells[index] = *cell;
        }

        Array<u32> rgb_words(kRecordCount);
        constexpr std::array<float, kRecordCount> rgb_values{
            0.0f, 0x1p-25f, 1.0f, 65408.0f, 1.0e-9f, 511.0f, 0.333f, 1234.5f, 65.0f, 4.0f, 0.5f, 3.0f, 100.0f};
        for (u32 index = 0u; index < kRecordCount; ++index) {
            const auto word = packRGB9E5(float3{rgb_values[index], rgb_values[index] * 0.5f, 0.0f});
            PPR_TEST_ASSERT(word.has_value());
            rgb_words[index] = *word;
        }

        const auto interval_buffer = makeStorageBuffer(
            device, kRecordCount * sizeof(PackedInterval), sizeof(PackedInterval), intervals.data());
        const auto ch_buffer = makeStorageBuffer(
            device, kRecordCount * sizeof(PackedCH), sizeof(PackedCH), fine_ch.data());
        const auto source_buffer = makeStorageBuffer(
            device, kRecordCount * sizeof(SourceMaterial), sizeof(SourceMaterial), sources.data());
        const auto cell_buffer = makeStorageBuffer(device, kRecordCount * sizeof(u32), sizeof(u32), cells.data());
        const auto rgb_buffer = makeStorageBuffer(device, kRecordCount * sizeof(u32), sizeof(u32), rgb_words.data());

        const auto interval_output = makeStorageBuffer(device, kRecordCount * 4u * sizeof(float), 16u);
        const auto ch_output = makeStorageBuffer(device, kRecordCount * 9u * 4u * sizeof(float), 16u);
        const auto emission_output = makeStorageBuffer(device, kRecordCount * 4u * sizeof(float), 16u);
        const auto diffuse_output = makeStorageBuffer(device, kRecordCount * 4u * sizeof(float), 16u);
        const auto cell_output = makeStorageBuffer(device, kRecordCount * 2u * sizeof(float), 8u);
        const auto rgb_output = makeStorageBuffer(device, kRecordCount * 4u * sizeof(float), 16u);

        setCodecProbe(*pipeline.m_root, kRecordCount);
        bindBuffer(*pipeline.m_root, "g_codec_intervals", interval_buffer);
        bindBuffer(*pipeline.m_root, "g_codec_fine_ch", ch_buffer);
        bindBuffer(*pipeline.m_root, "g_codec_source_materials", source_buffer);
        bindBuffer(*pipeline.m_root, "g_codec_cells", cell_buffer);
        bindBuffer(*pipeline.m_root, "g_codec_rgb9e5", rgb_buffer);
        bindBuffer(*pipeline.m_root, "g_codec_interval_out", interval_output);
        bindBuffer(*pipeline.m_root, "g_codec_ch_out", ch_output);
        bindBuffer(*pipeline.m_root, "g_codec_source_emission_out", emission_output);
        bindBuffer(*pipeline.m_root, "g_codec_source_diffuse_out", diffuse_output);
        bindBuffer(*pipeline.m_root, "g_codec_cell_out", cell_output);
        bindBuffer(*pipeline.m_root, "g_codec_rgb9e5_out", rgb_output);

        auto encoder = makeEncoder(fixture);
        {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(pipeline.m_pipeline, pipeline.m_root);
            pass->dispatchCompute((kRecordCount + 63u) / 64u, 1u, 1u);
            pass->end();
        }
        submit(fixture, *encoder);

        Array<float> gpu_intervals(kRecordCount * 4u);
        Array<float> gpu_ch(kRecordCount * 9u * 4u);
        Array<float> gpu_emission(kRecordCount * 4u);
        Array<float> gpu_diffuse(kRecordCount * 4u);
        Array<float> gpu_cells(kRecordCount * 2u);
        Array<float> gpu_rgb(kRecordCount * 4u);
        PPR_TEST_ASSERT(not make_error_code(
            device.readBuffer(interval_output, 0u, gpu_intervals.size() * sizeof(float), gpu_intervals.data())));
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(ch_output, 0u, gpu_ch.size() * sizeof(float), gpu_ch.data())));
        PPR_TEST_ASSERT(not make_error_code(
            device.readBuffer(emission_output, 0u, gpu_emission.size() * sizeof(float), gpu_emission.data())));
        PPR_TEST_ASSERT(not make_error_code(
            device.readBuffer(diffuse_output, 0u, gpu_diffuse.size() * sizeof(float), gpu_diffuse.data())));
        PPR_TEST_ASSERT(not make_error_code(
            device.readBuffer(cell_output, 0u, gpu_cells.size() * sizeof(float), gpu_cells.data())));
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(rgb_output, 0u, gpu_rgb.size() * sizeof(float), gpu_rgb.data())));

        for (u32 index = 0u; index < kRecordCount; ++index) {
            const Interval expected = unpackInterval(intervals[index]);
            PPR_TEST_ASSERT(gpu_intervals[index * 4u] == expected.m_radiance.x);
            PPR_TEST_ASSERT(gpu_intervals[index * 4u + 1u] == expected.m_radiance.y);
            PPR_TEST_ASSERT(gpu_intervals[index * 4u + 2u] == expected.m_radiance.z);
            PPR_TEST_ASSERT(gpu_intervals[index * 4u + 3u] == expected.m_transmittance);

            for (u32 coefficient = 0u; coefficient < 9u; ++coefficient) {
                const float3 expected_coefficient = decoded_ch[index].m_coefficients[coefficient];
                const float *const gpu = gpu_ch.data() + (index * 9u + coefficient) * 4u;
                const float tolerance = 1.0e-6f * (std::max)({std::abs(expected_coefficient.x),
                                                              std::abs(expected_coefficient.y),
                                                              std::abs(expected_coefficient.z), 1.0f});
                PPR_TEST_ASSERT(std::abs(gpu[0] - expected_coefficient.x) <= tolerance);
                PPR_TEST_ASSERT(std::abs(gpu[1] - expected_coefficient.y) <= tolerance);
                PPR_TEST_ASSERT(std::abs(gpu[2] - expected_coefficient.z) <= tolerance);
            }

            const float3 expected_emission = unpackRGB9E5(sources[index].m_emission);
            const float3 expected_diffuse = unpackRGB9E5(sources[index].m_diffuse);
            PPR_TEST_ASSERT(gpu_emission[index * 4u] == expected_emission.x);
            PPR_TEST_ASSERT(gpu_emission[index * 4u + 1u] == expected_emission.y);
            PPR_TEST_ASSERT(gpu_emission[index * 4u + 2u] == expected_emission.z);
            PPR_TEST_ASSERT(gpu_diffuse[index * 4u] == expected_diffuse.x);
            PPR_TEST_ASSERT(gpu_diffuse[index * 4u + 1u] == expected_diffuse.y);
            PPR_TEST_ASSERT(gpu_diffuse[index * 4u + 2u] == expected_diffuse.z);

            const float expected_temperature = halfValue(static_cast<u16>(cells[index] >> 16u));
            PPR_TEST_ASSERT(gpu_cells[index * 2u] == static_cast<float>(cells[index] & 0xffffu));
            PPR_TEST_ASSERT(gpu_cells[index * 2u + 1u] == expected_temperature);

            const float3 expected_rgb = unpackRGB9E5(rgb_words[index]);
            PPR_TEST_ASSERT(gpu_rgb[index * 4u] == expected_rgb.x);
            PPR_TEST_ASSERT(gpu_rgb[index * 4u + 1u] == expected_rgb.y);
            PPR_TEST_ASSERT(gpu_rgb[index * 4u + 2u] == expected_rgb.z);
        }

        _.log(std::format("WORLD_LIGHTING_CODEC cpu_gpu_decode_records={} agreement=true", kRecordCount));
    };

    // ------------------------------------------------------------------
    // committed thermal table and binary64 reference
    // ------------------------------------------------------------------

    [[nodiscard]] Array<u8> readThermalTable() {
        Array<u8> bytes(kThermalLutBytes);
        std::ifstream file{std::filesystem::current_path() / "lighting" / "thermal_lut.bin", std::ios::binary};
        PPR_TEST_ASSERT(file.is_open());
        file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        PPR_TEST_ASSERT(file.gcount() == static_cast<std::streamsize>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] double tableValue(const Array<u8> &bytes, const u32 row, const u32 channel) noexcept {
        u32 bits{};
        std::memcpy(&bits, bytes.data() + row * kThermalLutStride + channel * sizeof(u32), sizeof(bits));
        return static_cast<double>(std::bit_cast<float>(bits));
    }

    [[nodiscard]] RgbReference sampleThermalTable(const Array<u8> &bytes, const double kelvin) noexcept {
        if (kelvin <= kThermalLutMinKelvin) {
            return {};
        }

        const double q = (kThermalLutRows - 1u) *
                         std::log(kelvin / kThermalLutMinKelvin) /
                         std::log(kThermalLutMaxKelvin / kThermalLutMinKelvin);
        const u32 lower = (std::min) (static_cast<u32>(std::floor(q)), kThermalLutRows - 1u);
        const u32 upper = (std::min) (lower + 1u, kThermalLutRows - 1u);
        const float fraction = static_cast<float>(q - static_cast<double>(lower));

        const double weight = static_cast<double>(fraction);
        const auto interpolate = [&](const u32 channel) {
            return tableValue(bytes, lower, channel) +
                   (tableValue(bytes, upper, channel) - tableValue(bytes, lower, channel)) * weight;
        };
        return {interpolate(0u), interpolate(1u), interpolate(2u)};
    }

    PPR_UNIT_TEST (binary64_reference_calibration_and_committed_table) {
        const double reference_y = calibrationY();
        PPR_TEST_ASSERT(std::abs(reference_y - 4'498'563.65869) <= 1.0e-2);
        const double normalized_y = integrateXyz(kCalibrationKelvin)[1] / reference_y;
        PPR_TEST_ASSERT(std::abs(normalized_y - 1.0) <= 1.0e-12);

        const std::array<std::pair<double, double>, 7u> reference_luminance{{
            {1500.0, 0.0000025192},
            {2000.0, 0.000150939},
            {3000.0, 0.00983498},
            {4095.0, 0.0954911},
            {6500.0, 1.0},
            {10000.0, 4.31582},
            {65504.0, 112.467},
        }};
        for (const auto &[kelvin, expected]: reference_luminance) {
            const double luminance = referenceLuminance(engineReference(kelvin));
            PPR_TEST_ASSERT(std::abs(luminance - expected) <= std::abs(expected) * 1.0e-4 + 1.0e-12);
        }

        const Array<u8> table = readThermalTable();
        PPR_TEST_ASSERT(table.size() == kThermalLutBytes);
        PPR_TEST_ASSERT(table.size() % kThermalLutStride == 0u);
        for (u32 row = 0u; row < kThermalLutRows; ++row) {
            for (u32 byte = 12u; byte < kThermalLutStride; ++byte) {
                PPR_TEST_ASSERT(table[row * kThermalLutStride + byte] == 0u);
            }
        }

        // The committed rows retain absolute brightness and are calibrated, not
        // normalized per temperature.
        const RgbReference at_6500 = sampleThermalTable(table, 6500.0);
        PPR_TEST_ASSERT(std::abs(referenceLuminance(at_6500) - 1.0) <= 1.0e-3);
        const RgbReference at_10000 = sampleThermalTable(table, 10000.0);
        PPR_TEST_ASSERT(std::abs(referenceLuminance(at_10000) - 4.31582) <= 4.31582 * 1.0e-3);
        const RgbReference at_3000 = sampleThermalTable(table, 3000.0);
        PPR_TEST_ASSERT(std::abs(referenceLuminance(at_3000) - 0.00983498) <= 0.00983498 * 1.0e-3);
        PPR_TEST_ASSERT(sampleThermalTable(table, 256.0).m_r == 0.0);

        const double last_row_r = tableValue(table, kThermalLutRows - 1u, 0u);
        const double last_row_g = tableValue(table, kThermalLutRows - 1u, 1u);
        const double last_row_b = tableValue(table, kThermalLutRows - 1u, 2u);
        PPR_TEST_ASSERT(std::abs(last_row_r - 77.0781) <= 77.0781 * 1.0e-3);
        PPR_TEST_ASSERT(std::abs(last_row_g - 110.264) <= 110.264 * 1.0e-3);
        PPR_TEST_ASSERT(std::abs(last_row_b - 238.496) <= 238.496 * 1.0e-3);

        _.log(
            std::format(
                "WORLD_LIGHTING_CODEC y6500={:.9f} table_bytes={} row65504=({:.5f},{:.5f},{:.5f})", reference_y,
                table.size(), last_row_r, last_row_g, last_row_b));
    };

    // ------------------------------------------------------------------
    // every finite nonnegative binary16 Kelvin through the GPU table
    // ------------------------------------------------------------------

    struct ThermalProbeParameters {
        u32 m_count{};
        u32 m_reserved[3]{};
    };

    static_assert(sizeof(ThermalProbeParameters) == 16u);

    void setThermalProbe(rhi::IShaderObject &root, const u32 count) {
        const ThermalProbeParameters parameters{.m_count = count};
        rhi::ShaderCursor cursor{&root};
        rhi::ShaderCursor constants{};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_thermal_probe"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
    }

    PPR_UNIT_TEST (gpu_thermal_sampling_matches_binary64_for_every_half_kelvin) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        auto &device = fixture.device();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "thermal_emission_probe.slang",
            "thermal_emission_probe", module.writeRef()));

        constexpr const char *const entries[] = {"sampleThermal"};
        const auto program = makeProgram(device, module, entries);
        auto pipeline = makeComputePipeline(device, program);

        const Array<u8> table = readThermalTable();
        PPR_TEST_ASSERT(table.size() == kThermalLutBytes);

        constexpr u32 kFiniteHalfCount = 0x7c00u;
        Array<u32> half_bits(kFiniteHalfCount);
        for (u32 bits = 0u; bits < kFiniteHalfCount; ++bits) {
            half_bits[bits] = bits;
        }

        const auto table_buffer = makeStorageBuffer(device, kThermalLutBytes, kThermalLutStride, table.data());
        const auto half_buffer = makeStorageBuffer(
            device, kFiniteHalfCount * sizeof(u32), sizeof(u32), half_bits.data());
        const auto output_buffer = makeStorageBuffer(device, kFiniteHalfCount * 4u * sizeof(float), 16u);

        setThermalProbe(*pipeline.m_root, kFiniteHalfCount);
        bindBuffer(*pipeline.m_root, "g_thermal_lut", table_buffer);
        bindBuffer(*pipeline.m_root, "g_thermal_half_bits", half_buffer);
        bindBuffer(*pipeline.m_root, "g_thermal_output", output_buffer);

        auto encoder = makeEncoder(fixture);
        {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(pipeline.m_pipeline, pipeline.m_root);
            pass->dispatchCompute((kFiniteHalfCount + 63u) / 64u, 1u, 1u);
            pass->end();
        }
        submit(fixture, *encoder);

        Array<float> output(kFiniteHalfCount * 4u);
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(output_buffer, 0u, output.size() * sizeof(float), output.data())));

        double maximum_relative_error = 0.0;
        double maximum_absolute_error = 0.0;
        double maximum_rgb9e5_relative_error = 0.0;
        float maximum_kelvin = 0.0f;
        for (u32 bits = 0u; bits < kFiniteHalfCount; ++bits) {
            const float kelvin = halfValue(static_cast<u16>(bits));
            const float red = output[bits * 4u];
            const float green = output[bits * 4u + 1u];
            const float blue = output[bits * 4u + 2u];

            if (kelvin <= static_cast<float>(kThermalLutMinKelvin)) {
                PPR_TEST_ASSERT(red == 0.0f and green == 0.0f and blue == 0.0f);
                continue;
            }

            const RgbReference exact = engineReference(static_cast<double>(kelvin));
            const double maximum_exact = (std::max) ({exact.m_r, exact.m_g, exact.m_b});
            const double component_error = (std::max)({std::abs(static_cast<double>(red) - exact.m_r),
                                                        std::abs(static_cast<double>(green) - exact.m_g),
                                                        std::abs(static_cast<double>(blue) - exact.m_b)});
            const double relative_error = component_error / (std::max) (maximum_exact, 1.0e-6);
            PPR_TEST_ASSERT(relative_error <= 1.0e-3);

            if (relative_error > maximum_relative_error) {
                maximum_relative_error = relative_error;
                maximum_kelvin = kelvin;
            }
            maximum_absolute_error = (std::max) (maximum_absolute_error, component_error);

            const Expected<u32> quantized = packRGB9E5(
                float3{static_cast<float>(exact.m_r), static_cast<float>(exact.m_g), static_cast<float>(exact.m_b)});
            PPR_TEST_ASSERT(quantized.has_value());
            const float3 decoded = unpackRGB9E5(*quantized);
            const double maximum_decoded = (std::
                    max) ({static_cast<double>(decoded.x), static_cast<double>(decoded.y), static_cast<double>(decoded.z)});
            const double quantization_error =
                (std::max)({std::abs(static_cast<double>(decoded.x) - exact.m_r),
                            std::abs(static_cast<double>(decoded.y) - exact.m_g),
                            std::abs(static_cast<double>(decoded.z) - exact.m_b)});
            maximum_rgb9e5_relative_error = (std::max) (maximum_rgb9e5_relative_error,
                quantization_error / (std::max) (maximum_decoded, 1.0e-6));

            if (kelvin == 6500.0f) {
                const double luminance = 0.2126 * static_cast<double>(red) +
                                         0.7152 * static_cast<double>(green) +
                                         0.0722 * static_cast<double>(blue);
                PPR_TEST_ASSERT(std::abs(luminance - 1.0) <= 1.0e-3);
            }
        }

        PPR_TEST_ASSERT(maximum_relative_error <= 1.0e-3);

        _.log(
            std::format(
                "WORLD_LIGHTING_CODEC thermal_half_values={} max_relative={:.9f} at_kelvin={:.5f} max_absolute={:.9f} "
                "rgb9e5_max_relative={:.9f}",
                kFiniteHalfCount, maximum_relative_error, maximum_kelvin, maximum_absolute_error,
                maximum_rgb9e5_relative_error));
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // world-lighting codecs and thermal table
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_codecTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_codec") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingCodec::record_sizes_and_exact_zero_encoding,
                detail::WorldLightingCodec::interval_half_rounding_ties_subnormals_and_maximum,
                detail::WorldLightingCodec::rgb9e5_exponent_boundaries_ties_and_range,
                detail::WorldLightingCodec::ch_signed_ac_endpoints_scale_zero_and_dc_range,
                detail::WorldLightingCodec::cell_rounds_to_binary16_and_rejects_invalid_kelvin,
                detail::WorldLightingCodec::source_material_multiplies_emission_in_binary64_and_zeroes_metals,
                detail::WorldLightingCodec::last_trace_frame_follows_the_four_frame_schedule,
                detail::WorldLightingCodec::gpu_reflection_matches_record_strides_and_word_offsets,
                detail::WorldLightingCodec::gpu_decodes_every_record_identically_to_the_cpu_codec,
                detail::WorldLightingCodec::binary64_reference_calibration_and_committed_table,
                detail::WorldLightingCodec::gpu_thermal_sampling_matches_binary64_for_every_half_kelvin,
            });
        };
        return group;
    }
}
