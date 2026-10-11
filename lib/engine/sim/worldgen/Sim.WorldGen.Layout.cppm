module;
export module engine.sim:worldgen.layout;
import :worldgen.plan;
import engine.core;
import std;

export namespace pP::sim {
    enum class EGenerationErrc {
        constraints_exhausted = 1,
        segment_limit,
        room_anchor_limit,
        raster_reference_limit,
        matter_component_limit,
        matter_search_limit,
        matter_span_limit,
        matter_region_limit,
        matter_anchor_solid,
        matter_not_sealed,
        start_contour_limit,
        start_invalid
    };
    [[nodiscard]] const std::error_category &generationErrorCategory() noexcept;
    [[nodiscard]] std::error_code make_error_code(EGenerationErrc error) noexcept;

    struct GenerationSite {
        double m_x_m{}, m_y_m{};
        u32 m_biome{};
        constexpr bool operator==(const GenerationSite &) const noexcept = default;
    };
    /// Owned macro layout and fixed-size spatial index. Site order is its stable
    /// identity; exact squared-distance ties select the lowest site identity.
    class BiomeLayout {
        static constexpr u32 kBinsPerEdge = 64u;
        Array<GenerationSite> m_sites;
        Array<i32> m_next;
        std::array<i32, kBinsPerEdge * kBinsPerEdge> m_heads;
        double m_extent_m{};
        explicit BiomeLayout(double extent_m);
        void append(GenerationSite site);
        friend Expected<BiomeLayout> compileBiomeLayout(const GenerationPlan &, const IContext *);
        friend Expected<BiomeLayout> restoreBiomeLayout(const GenerationPlan &, std::span<const GenerationSite>);

    public:
        BiomeLayout(BiomeLayout &&) noexcept = default;
        BiomeLayout &operator=(BiomeLayout &&) noexcept = default;
        BiomeLayout(const BiomeLayout &) = delete;
        BiomeLayout &operator=(const BiomeLayout &) = delete;
        [[nodiscard]] std::span<const GenerationSite> sites() const noexcept;
        /// Finite positions may lie outside the world after domain warp, within
        /// twice kNoiseCoordinateLimitMetres on each axis.
        [[nodiscard]] u32 nearest(double x_m, double y_m) const noexcept;
    };
    [[nodiscard]] Expected<BiomeLayout> compileBiomeLayout(
        const GenerationPlan &plan, const IContext *context = nullptr);
    /// Rebuilds only the derived spatial index from structurally validated saved sites.
    [[nodiscard]] Expected<BiomeLayout> restoreBiomeLayout(
        const GenerationPlan &plan, std::span<const GenerationSite> sites);
    /// Layout must have been compiled from this plan. Sample coordinates are
    /// finite world metres within kNoiseCoordinateLimitMetres on each axis.
    [[nodiscard]] u32 generationBiomeAt(
        const GenerationPlan &plan, const BiomeLayout &layout, double x_m, double y_m) noexcept;
}
