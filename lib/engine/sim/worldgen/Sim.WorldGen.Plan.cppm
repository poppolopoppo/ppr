module;
export module engine.sim:worldgen.plan;
import :worldgen.noise;
import engine.core;
import std;

export namespace pP::sim {
    inline constexpr u32 kGeneratorAlgorithmVersion = 6u;
    inline constexpr u32 kMaxGenerationMaterials = 256u;
    inline constexpr u32 kMaxGenerationFields = 256u;
    inline constexpr u32 kMaxGenerationBiomes = 64u;
    inline constexpr u32 kMaxGenerationFeatures = 128u;
    inline constexpr u32 kMaxGenerationRules = 128u;
    inline constexpr u32 kMaxGenerationSites = 4096u;
    inline constexpr u32 kGenerationWormOriginAttempts = 4096u;
    inline constexpr u32 kGenerationBlobOriginAttempts = 256u;
    inline constexpr u32 kMaxGenerationPoreRadiusFine = 32u;
    // Bound the complete rejection-sampling budget before any geometry allocation.
    inline constexpr u64 kMaxGenerationOriginAttempts = 4u * 1024u * 1024u;

    // Numeric owned input. Keys are stable stream identities compiled by the
    // caller; array indices are resolved references, never persistent identities.
    enum class EGenerationMatter : u8 {
        solid,
        liquid,
        gas
    };
    struct GenerationMaterial {
        u16 m_id{};
        bool m_terrain{}, m_diggable{};
        EGenerationMatter m_matter{};
        double m_reference_density_kg_m3{};
        u64 m_key{};
    };
    struct GenerationField {
        u64 m_key{};
        NoiseFieldDesc m_noise{};
    };
    struct GenerationChoice {
        u64 m_key{};
        u16 m_material{};
        double m_weight{};
    };
    struct GenerationBand {
        u64 m_key{};
        u32 m_order{}, m_field{};
        double m_depth_min{}, m_depth_max{};
        Array<GenerationChoice> m_choices{};
        // Derived by compileGenerationPlan, never trusted from caller input.
        double m_total_weight{};
        double m_bed_period_m{}, m_bed_warp_m{}, m_bed_dip{};
    };
    enum class EGenerationDepositShape : u8 {
        field,
        blob
    };
    enum class EGenerationDepositStage : u8 {
        bulk,
        detail
    };
    struct GenerationDeposit {
        u64 m_key{};
        u32 m_order{}, m_field{};
        u16 m_material{}, m_host{};
        double m_depth_min{}, m_depth_max{}, m_threshold_min{}, m_threshold_max{};
        EGenerationDepositShape m_shape{};
        EGenerationDepositStage m_stage{};
        u32 m_blob_count{};
        double m_radius_min_m{}, m_radius_max_m{}, m_stretch_min{1.0}, m_stretch_max{1.0}, m_distortion{};
        double m_core_coverage{1.0}, m_fringe_coverage{1.0}, m_sprinkle_probability{};
        double m_orientation_min_degrees{}, m_orientation_max_degrees{360.0};
    };
    struct GenerationBiome {
        u64 m_key{};
        u32 m_temperature_field{};
        double m_temperature_base_k{}, m_temperature_variation_k{};
        Array<GenerationBand> m_bands{};
        Array<GenerationDeposit> m_deposits{};
        u16 m_lining_material{};
        u32 m_lining_thickness_fine{};
        u32 m_relaxation_passes{};
        double m_relaxation_adoption{0.5};
        double m_sprinkle_probability{};
        u32 m_pore_field{};
        double m_pore_probability{};
        double m_pore_radius_min_m{2.0};
        double m_pore_radius_max_m{3.0};
        double m_pore_affinity_min{};
        u32 m_pore_passes{};
        u32 m_finish_passes{};
        double m_finish_adoption{0.15};
        u16 m_soil_material{};
        u16 m_weathered_material{};
        double m_soil_depth_m{};
        double m_weathered_depth_m{};
        u16 m_formation_material{};
        u16 m_rind_material{};
        double m_formation_probability{};
        double m_formation_length_m{3.0};
        u32 m_erosion_passes{};
    };
    enum class EGenerationShape : u8 {
        rectangle,
        ellipse,
        capsule
    };
    enum class EGenerationMarker : u8 {
        start,
        traversal_room,
        sealed_pocket,
        resource_anchor,
        inert
    };
    struct GenerationBounds {
        i32 m_min_x{}, m_min_y{}, m_max_x{}, m_max_y{};
    };
    struct GenerationOperation {
        u64 m_key{};
        u32 m_order{};
        EGenerationShape m_shape{};
        bool m_carve{};
        i32 m_x{}, m_y{}, m_end_x{}, m_end_y{};
        u32 m_width{}, m_height{};
        double m_radius_m{};
        u16 m_material{};
    };
    struct GenerationMarker {
        u64 m_key{};
        EGenerationMarker m_role{};
        i32 m_x{}, m_y{};
    };
    struct GenerationFeature {
        u64 m_key{};
        GenerationBounds m_bounds{};
        bool m_allow_reflection{};
        Array<GenerationOperation> m_operations{};
        Array<GenerationMarker> m_markers{};
    };
    struct GenerationBiomeConstraint {
        u64 m_key{};
        u32 m_biome{}, m_required_count{};
        double m_weight{}, m_depth_min{}, m_depth_max{}, m_distance_min_m{}, m_distance_max_m{};
    };
    struct GenerationResource {
        u64 m_key{};
        u32 m_marker{}, m_minimum_fine_cells{};
        u16 m_material{};
        double m_max_distance_m{};
    };
    struct GenerationCave {
        u32 m_biome{}, m_field{};
        double m_depth_min{}, m_depth_max{}, m_threshold_min{}, m_threshold_max{};
    };
    struct GenerationWorm {
        u32 m_biome{}, m_count{};
        bool m_carve{};
        double m_depth_min{}, m_depth_max{}, m_length_min_m{}, m_length_max_m{}, m_radius_min_m{}, m_radius_max_m{},
            m_turn_probability{};
        u16 m_material{}, m_host{};
        double m_turn_smoothing{}, m_radius_variation{}, m_chamber_probability{}, m_chamber_scale{1.0};
    };
    struct GenerationConnection {
        double m_radius_m{};
        u32 m_extra_links{};
        u32 m_natural_anchors{}, m_wells{}, m_branches{};
        double m_roughness_m{};
    };
    struct GenerationPlacement {
        u32 m_feature{}, m_biome{}, m_min_count{}, m_max_count{}, m_attempts{}, m_priority{};
        double m_depth_min{}, m_depth_max{}, m_spacing_m{};
        bool m_reflect{};
    };
    struct GenerationFill {
        u32 m_placement_rule{}, m_marker{};
        u16 m_material{};
        bool m_gas{};
        double m_temperature_k{}, m_bulk_density_kg_m3{};
        std::optional<double> m_fill_height_m{};
    };
    enum class EGenerationPhase : u8 {
        deposit,
        carve,
        stamp,
        fill
    };
    struct GenerationRule {
        u64 m_key{};
        u32 m_order{};
        EGenerationPhase m_phase{};
        std::variant<GenerationCave, GenerationWorm, GenerationConnection, GenerationPlacement, GenerationFill>
            m_parameters{};
    };
    struct GenerationRecipe {
        Array<GenerationMaterial> m_materials{};
        Array<GenerationField> m_fields{};
        Array<GenerationBiome> m_biomes{};
        Array<GenerationFeature> m_features{};
        Array<GenerationBiomeConstraint> m_constraints{};
        Array<GenerationRule> m_rules{};
        Array<GenerationResource> m_resources{};
        double m_cell_size_m{}, m_reference_thickness_m{};
        u16 m_boundary_material{};
        u32 m_boundary_fine{}, m_min_sites{}, m_max_sites{}, m_site_attempts{};
        double m_site_spacing_m{}, m_boundary_warp_amplitude_m{};
        u32 m_warp_x_field{}, m_warp_y_field{};
        u32 m_start_biome{}, m_start_feature{}, m_start_marker{}, m_start_x_fine{}, m_start_y_fine{};
        double m_start_clearance_m{}, m_start_route_width_m{}, m_actor_half_width_m{}, m_actor_half_height_m{};
        // Disabled in small legacy fixtures; authored worlds use an open surface.
        bool m_open_surface{};
        u32 m_surface_field{}, m_bedrock_field{};
        double m_surface_elevation_m{}, m_surface_amplitude_m{}, m_bedrock_thickness_m{}, m_bedrock_variation_m{};
        u16 m_bedrock_material{};
        std::optional<double> m_start_depth_m{};
    };

    // Fixed SplitMix64 stream with unbiased bounded integer draws. Domain keys
    // and ordinals isolate independent decisions from unrelated content changes.
    [[nodiscard]] u64 generationStreamSeed(u64 seed, u64 domain_key, u64 ordinal) noexcept;
    class GenerationRandom {
        u64 m_state{};

    public:
        explicit GenerationRandom(u64 state) noexcept;
        [[nodiscard]] u64 next() noexcept;
        [[nodiscard]] u64 bounded(u64 exclusive_max) noexcept;
        [[nodiscard]] double unit() noexcept;
    };

    class GenerationPlan {
        GenerationRecipe m_recipe;
        Array<NoiseField> m_fields;
        u64 m_seed{};
        GenerationPlan(GenerationRecipe recipe, Array<NoiseField> fields, u64 seed) noexcept;
        friend Expected<GenerationPlan> compileGenerationPlan(GenerationRecipe, u64);

    public:
        GenerationPlan(GenerationPlan &&) noexcept = default;
        GenerationPlan &operator=(GenerationPlan &&) noexcept = default;
        GenerationPlan(const GenerationPlan &) = delete;
        GenerationPlan &operator=(const GenerationPlan &) = delete;
        [[nodiscard]] const GenerationRecipe &recipe() const noexcept;
        [[nodiscard]] std::span<const NoiseField> fields() const noexcept;
        [[nodiscard]] u64 seed() const noexcept;
        /// Owned vector payload capacities; excludes opaque compiled-noise allocations.
        [[nodiscard]] u64 retainedPayloadBytes() const noexcept;
    };
    [[nodiscard]] double generationSurfaceHeight(const GenerationPlan &plan, double x_m) noexcept;
    [[nodiscard]] double generationBedrockHeight(const GenerationPlan &plan, double x_m) noexcept;
    [[nodiscard]] double generationDepth(const GenerationPlan &plan, double x_m, double y_m) noexcept;
    [[nodiscard]] bool generationSubsurfaceBounds(
        const GenerationPlan &plan, i64 min_x, i64 min_y, i64 max_x, i64 max_y) noexcept;
    [[nodiscard]] std::error_code validateGenerationRecipe(const GenerationRecipe &recipe);
    [[nodiscard]] Expected<GenerationPlan> compileGenerationPlan(GenerationRecipe recipe, u64 seed);
}
