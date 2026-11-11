module;
#include "pP/Macros.h"
export module engine.app:renderer.world_lighting;

import :renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
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
    // full-domain visibility records
    // ------------------------------------------------------------------
    // One 8-byte record holds an 8x8 occupancy mask. Levels 0/1/2 cover
    // native, 2x2 and 4x4 cells per bit, so a native cell's level-l bit is at
    // `((x >> l) % 8) + 8*((y >> l) % 8)`. Level record offsets and row pitches
    // are the shared CPU/GPU addressing contract for one structured uint2
    // buffer with stride 8 (172,032 bytes).
    inline constexpr u32 kLightingEdge{1024u};
    inline constexpr std::array<u32, 3u> kVisibilityLevelOffsets{0u, 16'384u, 20'480u};
    inline constexpr std::array<u32, 3u> kVisibilityLevelPitches{128u, 64u, 32u};
    inline constexpr u32 kVisibilityRecordCount{21'504u};
    inline constexpr u64 kVisibilityBufferBytes{172'032u};
    static_assert(
        kVisibilityRecordCount == (kLightingEdge / 8u) * (kLightingEdge / 8u) +
                                      (kLightingEdge / 16u) * (kLightingEdge / 16u) +
                                      (kLightingEdge / 32u) * (kLightingEdge / 32u));
    static_assert(kVisibilityRecordCount * 8u == kVisibilityBufferBytes);

    struct VisibilityRecord {
        u32 m_low{};
        u32 m_high{};
    };

    static_assert(sizeof(VisibilityRecord) == 8u and alignof(VisibilityRecord) == 4u);
    static_assert(PPR_OFFSETOF(VisibilityRecord, m_low) == 0u and PPR_OFFSETOF(VisibilityRecord, m_high) == 4u);

    [[nodiscard]] constexpr u32 visibilityRecordIndex(
        const u32 level, const u32 record_x, const u32 record_y) noexcept {
        return kVisibilityLevelOffsets[level] + record_y * kVisibilityLevelPitches[level] + record_x;
    }

    // ------------------------------------------------------------------
    // coarse cascade geometry and persistent storage
    // ------------------------------------------------------------------
    // Five persistent interval levels live in one structured uint2 store
    // (stride 8): 1,048,576 directional records per level, 8 MiB per level and
    // 40 MiB total. Level l=1..5 has probe spacing 2<<l, 4<<(2*l) directions
    // per probe and a logical 1024-square atlas. L0 is represented by the fine
    // CH field, so no raw L0 interval allocation exists.
    inline constexpr u32 kCascadeLevels{5u};
    inline constexpr u32 kCascadeLevelRecords{1'048'576u};
    inline constexpr u64 kCascadeLevelBytes{8'388'608u};
    inline constexpr u32 kCascadeRecordCount{kCascadeLevels * kCascadeLevelRecords};
    inline constexpr u64 kCascadeBufferBytes{kCascadeLevels * kCascadeLevelBytes};
    inline constexpr u32 kCascadeTerminalLevel{kCascadeLevels};
    inline constexpr u32 kCascadeTerminalDirections{16'384u};
    // One quarter-probe logical dispatch domain per coarse level.
    inline constexpr u32 kCascadeDomainEdge{512u};
    inline constexpr u32 kCascadeDispatchThreads{16u};
    // Fixed reference penumbra width; it is not a retained control.
    inline constexpr float kCascadePenumbraWidth{0.5f};

    static_assert(kCascadeRecordCount == 5'242'880u and kCascadeBufferBytes == 41'943'040u);
    static_assert(kCascadeLevelRecords * sizeof(PackedInterval) == kCascadeLevelBytes);

    // One RGB9E5 outgoing-radiance word per native cell.
    inline constexpr u32 kSourceCellBytes{kLightingEdge * kLightingEdge * sizeof(u32)};
    // Fine CH field: 512 square, 32-byte records, one in-place field.
    inline constexpr u32 kFineCHEdge{512u};
    inline constexpr u32 kFineCHRecordCount{kFineCHEdge * kFineCHEdge};
    inline constexpr u64 kFineCHBytes{static_cast<u64>(kFineCHRecordCount) * sizeof(PackedCH)};

    static_assert(kSourceCellBytes == 4'194'304u and kFineCHBytes == 8'388'608u);

    // Approved accumulation rate: next = lerp(previous, reconstructed,
    // 1 - exp(-2 * delta_seconds)) with the learning-rate multiplier fixed at
    // one. This is an implementation constant, not a display control. The
    // caller supplies elapsed active presentation time; paused simulation does
    // not stop accumulation, while suspended/minimized intervals are excluded
    // by the injected active-time value.
    inline constexpr double kFineAccumulationRate{2.0};

    [[nodiscard]] inline float fineAccumulationWeight(const double delta_seconds) noexcept {
        return static_cast<float>(1.0 - std::exp(-kFineAccumulationRate * delta_seconds));
    }

    // Probe spacing, direction count and probe-grid edge for one cascade level.
    [[nodiscard]] constexpr u32 cascadeSpacing(const u32 level) noexcept {
        return 2u << level;
    }

    [[nodiscard]] constexpr u32 cascadeDirections(const u32 level) noexcept {
        return 4u << (2u * level);
    }

    [[nodiscard]] constexpr u32 cascadeProbeEdge(const u32 level) noexcept {
        return kLightingEdge / cascadeSpacing(level);
    }

    [[nodiscard]] constexpr u32 cascadeGroupEdge(const u32 level) noexcept {
        return cascadeProbeEdge(level) / 2u;
    }

    // Ray interval start in native-cell distance units, derived from the fixed
    // reference penumbra width: edge * penumbra_width * 1e-3 * 2^(2l-1) gives
    // 1.024, 4.096, 16.384, 65.536 and 262.144 cells for l=1..5. These are
    // distances, not buffer offsets. The finite end for l<5 is a_(l+1); L5
    // traces to the actual map exit.
    [[nodiscard]] constexpr float cascadeIntervalStart(const u32 level) noexcept {
        constexpr double cascade_0_length = static_cast<double>(kLightingEdge) *
                                            static_cast<double>(kCascadePenumbraWidth) *
                                            1.0e-3;
        return static_cast<float>(cascade_0_length * static_cast<double>(1u << (2u * level - 1u)));
    }

    // ------------------------------------------------------------------
    // categorical occupancy and native-cell traversal reference
    // ------------------------------------------------------------------
    // The visible palette marks optical-blend (receive-only) surfaces with bit
    // 0. A receive-only surface with no authored emission is transparent to
    // transport; material zero is empty and every other resolved material is
    // opaque regardless of brightness or emission.
    inline constexpr u32 kOpticalBlendSurfaceFlag{1u};

    [[nodiscard]] constexpr bool receiveOnlyCell(const u32 surface_flags, const float emissive_strength) noexcept {
        return (surface_flags & kOpticalBlendSurfaceFlag) != 0u and emissive_strength == 0.0f;
    }

    [[nodiscard]] constexpr bool resolvedCellOccupied(const u32 resolved, const bool receive_only) noexcept {
        return (resolved & 0xffffu) != 0u and not receive_only;
    }

    // Simple positive-length native-cell DDA over a classified occupancy grid.
    // Returns the first occupied cell with a positive-length intersection, or
    // nullopt for a clear, point-only, zero-length, nonfinite or non-advancing
    // reference segment. This is the test oracle: it never consults the GPU jump
    // table, and the GPU path reports the corresponding numeric failure through
    // the shared status record. Exact grid boundaries select the cell entered by
    // the direction, exact corners advance both axes and a crossing at t=1 is
    // excluded. A zero direction component on an exact boundary keeps the upper
    // (`floor`) cell.
    [[nodiscard]] std::optional<u32> traceNativeCellDDA(
        std::span<const u8> occupancy, u32 edge, float2 start, float2 end) noexcept;

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

    // ------------------------------------------------------------------
    // immutable per-world lighting publication inputs
    // ------------------------------------------------------------------

    // Strong owners only: the consumer never resolves a pass/service for these
    // buffers. m_terrain is the full-domain packed cell grid, m_visible_materials
    // is the admitted GridTerrainVisual palette, m_source_materials is the
    // eight-byte derived source palette and m_thermal_lut is the shared table.
    // m_prior_ch optionally supplies a known DC field for diagnostics; production
    // leaves it empty and source preparation reads the owner's single in-place
    // fine CH field before fine reconstruction overwrites it.
    struct WorldLightingInputs {
        pP::GridParticleGpuView m_particles{};
        pP::GridParticlePresentationIdentity m_identity{};
        rhi::ComPtr<rhi::IBuffer> m_terrain{};
        rhi::ComPtr<rhi::IBuffer> m_visible_materials{};
        rhi::ComPtr<rhi::IBuffer> m_source_materials{};
        rhi::ComPtr<rhi::IBuffer> m_thermal_lut{};
        rhi::ComPtr<rhi::IBuffer> m_prior_ch{};
        u32 m_material_count{};
    };
}

export namespace pP {
    // Plan spelling for the lighting owner; the record is defined alongside
    // the shared codecs so worlds and passes share one layout authority.
    using WorldLightingInputs = lighting::WorldLightingInputs;
}
