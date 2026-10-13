module;
export module engine.sim:worldgen.geometry;
import :worldgen.plan;
import :worldgen.layout;
import engine.core;
import std;

export namespace pP::sim {
    inline constexpr u32 kMaxGenerationSegments = 1024u * 1024u;
    inline constexpr u32 kGenerationStartRule = (std::numeric_limits<u32>::max)();
    struct GenerationInstance {
        u32 m_feature{}, m_rule{}, m_ordinal{};
        i32 m_origin_x{}, m_origin_y{};
        bool m_reflected{};
        constexpr bool operator==(const GenerationInstance &) const noexcept = default;
    };
    struct GenerationSegment {
        u32 m_rule{};
        double m_x0_m{}, m_y0_m{}, m_x1_m{}, m_y1_m{}, m_radius_m{};
        constexpr bool operator==(const GenerationSegment &) const noexcept = default;
    };
    struct GenerationGeometry {
        Array<GenerationInstance> m_instances;
        Array<GenerationSegment> m_segments;
        Array<u32> m_omitted_instances;
        u32 m_worst_placement_attempts{};
        struct Blob {
            u32 m_biome{}, m_deposit{}, m_ordinal{};
            double m_x_m{}, m_y_m{}, m_radius_m{}, m_stretch{}, m_angle{}, m_offset_x{}, m_offset_y{};
            constexpr bool operator==(const Blob &) const noexcept = default;
        };
        Array<Blob> m_blobs;
    };
    /// Derives all nonlocal features, worms and room links before any chunk
    /// rasterization. Required placement exhaustion never silently omits work.
    [[nodiscard]] Expected<GenerationGeometry> compileGenerationGeometry(
        const GenerationPlan &plan, const BiomeLayout &layout, const IContext *context = nullptr,
        u64 *known_scratch_peak = nullptr);
    /// Uniform-biome laboratory geometry: deposits and worms only. No start,
    /// feature reservations, traversal connections or initial matter are placed.
    [[nodiscard]] Expected<GenerationGeometry> compileBiomeSampleGeometry(
        const GenerationPlan &plan, const BiomeLayout &layout, const IContext *context = nullptr,
        u64 *known_scratch_peak = nullptr);
}
