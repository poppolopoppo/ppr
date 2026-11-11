module;
#include "pP/Macros.h"
export module engine.app:renderer.world_lighting;

import engine.core;
import engine.math;
import std;

export namespace pP::lighting {
    PPR_DECLARE_LOG_CATEGORY(WorldLighting)

    // ------------------------------------------------------------------
    // published record words
    // ------------------------------------------------------------------
    // Byte-identical to assets/shaders/world_lighting_codec.slang. The CPU
    // records are the publication/storage shape; CH and Interval are decoded
    // math values and are never uploaded as GPU records.

    struct PackedInterval {
        u32 m_rg{};
        u32 m_bt{};
    };

    static_assert(sizeof(PackedInterval) == 8u and alignof(PackedInterval) == 4u);
    static_assert(PPR_OFFSETOF(PackedInterval, m_rg) == 0u and PPR_OFFSETOF(PackedInterval, m_bt) == 4u);

    struct PackedCH {
        std::array<u32, 8u> m_words{};
    };

    static_assert(sizeof(PackedCH) == 32u and alignof(PackedCH) == 4u);

    struct SourceMaterial {
        u32 m_emission{};
        u32 m_diffuse{};
    };

    static_assert(sizeof(SourceMaterial) == 8u and alignof(SourceMaterial) == 4u);
    static_assert(PPR_OFFSETOF(SourceMaterial, m_emission) == 0u and PPR_OFFSETOF(SourceMaterial, m_diffuse) == 4u);

    static_assert(
        std::is_standard_layout_v<PackedInterval> and
        std::is_trivially_copyable_v<PackedInterval> and
        std::is_standard_layout_v<PackedCH> and
        std::is_trivially_copyable_v<PackedCH> and
        std::is_standard_layout_v<SourceMaterial> and
        std::is_trivially_copyable_v<SourceMaterial>);

    // ------------------------------------------------------------------
    // CPU math values
    // ------------------------------------------------------------------

    struct Interval {
        float3 m_radiance{float3{zero_v}};
        float m_transmittance{};
    };

    // C0 followed by cos1, sin1, cos2, sin2, cos3, sin3, cos4, sin4; each
    // coefficient keeps its RGB components together.
    struct CH {
        std::array<float3, 9u> m_coefficients;

        CH() noexcept {
            for (float3 &coefficient: m_coefficients) {
                coefficient = float3{zero_v};
            }
        }
    };

    // ------------------------------------------------------------------
    // codecs and admission
    // ------------------------------------------------------------------

    // Material ID zero canonicalizes the empty cell to an all-zero word.
    // Negative/NaN/Inf Kelvin is invalid; above 65,504 K is out of range.
    // Valid Kelvin rounds to IEEE binary16 nearest/ties-to-even and
    // canonicalizes -0 to +0.
    [[nodiscard]] Expected<u32> packCell(u16 material, double kelvin);

    // Shared-exponent RGB9E5: mantissas in bits 0..26, exponent in 27..31.
    [[nodiscard]] Expected<u32> packRGB9E5(float3 rgb);
    [[nodiscard]] float3 unpackRGB9E5(u32 word) noexcept;

    // Coarse interval words: word 0 = FP16 R/G, word 1 = FP16 B/T.
    [[nodiscard]] Expected<PackedInterval> packInterval(const Interval &value);
    [[nodiscard]] Interval unpackInterval(PackedInterval value) noexcept;

    // Fine CH: RGB9E5 C0, FP32 shared AC scale, six words of signed int8 AC.
    [[nodiscard]] Expected<PackedCH> packCH(const CH &value);
    [[nodiscard]] CH unpackCH(const PackedCH &value) noexcept;

    // Eight-byte source palette record. Emission is multiplied in binary64 and
    // range-checked before RGB9E5 encoding; pure metals have zero diffuse.
    [[nodiscard]] Expected<SourceMaterial> makeSourceMaterial(
        float3 emission, double strength, float3 tint, float3 texture_mean, float metallic);

    // Fixed four-frame probe schedule. group_phase is the group offset h in
    // [0,3]; local_probe is k = local_x + 2*local_y in [0,3]. Returns the
    // probe's actual last trace frame, or nullopt before its first update.
    [[nodiscard]] std::optional<u64> lastTraceFrame(u64 accepted_frame, u32 group_phase, u32 local_probe) noexcept;

    // ------------------------------------------------------------------
    // committed thermal table shape
    // ------------------------------------------------------------------

    inline constexpr u32 kThermalLutRows = 2048u;
    inline constexpr u32 kThermalLutStride = 16u;
    inline constexpr u32 kThermalLutBytes = kThermalLutRows * kThermalLutStride;
    inline constexpr double kThermalLutMinKelvin = 256.0;
    inline constexpr double kThermalLutMaxKelvin = 65504.0;
    static_assert(kThermalLutBytes == 32768u);
}
