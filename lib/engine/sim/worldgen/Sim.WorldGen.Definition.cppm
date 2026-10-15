module;
export module engine.sim:worldgen.definition;
import :worldgen.noise;
import :worldgen.plan;
import engine.core;
import std;


// ------------------------------------------------------------------
// definition schema
// ------------------------------------------------------------------

namespace pP::sim::definition::schema {
    [[nodiscard]] constexpr config::Metadata metadata(const std::string_view name) {
        return {
            name, "World generation definition", "World generation", config::EMutability::startup,
            config::ESource::file};
    }

    template<typename Owner, typename T>
    [[nodiscard]] constexpr auto collection(
        T Owner::*member, const std::string_view name, const std::size_t minimum, const std::size_t maximum,
        const config::EPresence presence = config::EPresence::required) {
        auto result = config::collection(member, metadata(name), {minimum, maximum});
        result.m_presence = presence;
        return result;
    }
}

// Canonical definition — file-neutral owned authoring fragment.
export namespace pP::sim::definition {
    using config::Diagnostics;
    using config::KeyedCollection;
    using config::EPresence;
    using config::field;
    using config::group;
    using config::FloatDomain;
    using config::Range;

    inline constexpr u32 kDefinitionSchemaVersion = 2u;

    // ------------------------------------------------------------------
    // references
    // ------------------------------------------------------------------

    struct FieldReference {
        std::string m_key{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(
                &FieldReference::m_key, schema::metadata("key"), config::StringDomain{128u, false},
                EPresence::required)};
        }
    };

    struct MaterialReference {
        std::string m_key{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(
                &MaterialReference::m_key, schema::metadata("key"), config::StringDomain{128u, false},
                EPresence::required)};
        }
    };

    inline constexpr config::ReferenceDomain<FieldReference> noise_reference{{"fields", "noise"}};
    inline constexpr config::ReferenceDomain<MaterialReference> terrain_reference{{"materials", "terrain"}};
    inline constexpr config::ReferenceDomain<MaterialReference> choice_reference{{"materials", "terrain_or_vacuum"}};

    struct BiomeReference {
        std::string m_key{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(
                &BiomeReference::m_key, schema::metadata("key"), config::StringDomain{128u, false},
                EPresence::required)};
        }
    };

    struct FeatureReference {
        std::string m_key{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(
                &FeatureReference::m_key, schema::metadata("key"), config::StringDomain{128u, false},
                EPresence::required)};
        }
    };

    struct ProfileReference {
        std::string m_key{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(
                &ProfileReference::m_key, schema::metadata("key"), config::StringDomain{128u, false},
                EPresence::required)};
        }
    };

    // Owners are supplied by the referring profile/feature, never encoded as a global fallback.
    struct RuleReference {
        std::string m_key{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(
                &RuleReference::m_key, schema::metadata("key"), config::StringDomain{128u, false},
                EPresence::required)};
        }
    };

    struct MarkerReference {
        std::string m_key{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(
                &MarkerReference::m_key, schema::metadata("key"), config::StringDomain{128u, false},
                EPresence::required)};
        }
    };
    inline constexpr config::ReferenceDomain<BiomeReference> biome_reference{{"biomes", "active"}};
    inline constexpr config::ReferenceDomain<FeatureReference> feature_reference{{"features", "template"}};
    inline constexpr config::ReferenceDomain<MaterialReference> particle_reference{{"materials", "particle"}};
    inline constexpr config::ReferenceDomain<RuleReference> placement_reference{
        {"rules", "placement", config::EReferenceScope::owner}};
    inline constexpr config::ReferenceDomain<MarkerReference> start_marker_reference{
        {"markers", "start", config::EReferenceScope::owner}};
    inline constexpr config::ReferenceDomain<MarkerReference> resource_marker_reference{
        {"markers", "resource_anchor", config::EReferenceScope::owner}};
    inline constexpr config::ReferenceDomain<MarkerReference> pocket_marker_reference{
        {"markers", "sealed_pocket", config::EReferenceScope::owner}};

    // ------------------------------------------------------------------
    // materials
    // ------------------------------------------------------------------

    enum class EMaterialRepresentation : u8 {
        terrain,
        particle
    };

    struct Material {
        EGenerationMatter m_phase{};
        EMaterialRepresentation m_representation{};
        std::optional<bool> m_diggable{};
        double m_density{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &Material::m_phase, schema::metadata("phase"),
                    config::EnumDomain<EGenerationMatter, 3>{
                        {config::EnumEntry{EGenerationMatter::solid, "solid"},
                            config::EnumEntry{EGenerationMatter::liquid, "liquid"},
                            config::EnumEntry{EGenerationMatter::gas, "gas"}}},
                    EPresence::required),
                field(
                    &Material::m_representation, schema::metadata("representation"),
                    config::EnumDomain<EMaterialRepresentation, 2>{
                        {config::EnumEntry{EMaterialRepresentation::terrain, "terrain"},
                            config::EnumEntry{EMaterialRepresentation::particle, "particle"}}},
                    EPresence::required),
                field(&Material::m_diggable, schema::metadata("diggable"), config::boolean_domain, EPresence::optional),
                field(
                    &Material::m_density, schema::metadata("density"),
                    FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}, EPresence::required)};
        }
    };

    struct Temperature {
        FieldReference m_field{};
        double m_base_k{}, m_variation_k{};

        [[nodiscard]] static constexpr auto describe() {
            constexpr double maximum = static_cast<double>((std::numeric_limits<float>::max)()) / 2.0;
            return std::tuple{
                field(&Temperature::m_field, schema::metadata("field"), noise_reference, EPresence::required),
                field(
                    &Temperature::m_base_k, schema::metadata("base_k"), FloatDomain{0.0, maximum, false, true},
                    EPresence::required),
                field(&Temperature::m_variation_k, schema::metadata("variation_k"), FloatDomain{0.0, maximum})};
        }
    };

    struct DepthInterval {
        double m_min{}, m_max{1.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&DepthInterval::m_min, schema::metadata("min"), FloatDomain{0.0, 1.0}, EPresence::required),
                field(&DepthInterval::m_max, schema::metadata("max"), FloatDomain{0.0, 1.0})};
        }
    };

    struct ThresholdInterval {
        double m_min{-1.0}, m_max{1.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&ThresholdInterval::m_min, schema::metadata("min"), FloatDomain{-1.0, 1.0}, EPresence::required),
                field(&ThresholdInterval::m_max, schema::metadata("max"), FloatDomain{-1.0, 1.0}, EPresence::required)};
        }
    };

    // Deposits require both authored depth endpoints, unlike a band's default upper endpoint.
    struct DepositDepthInterval {
        double m_min{}, m_max{1.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &DepositDepthInterval::m_min, schema::metadata("min"), FloatDomain{0.0, 1.0}, EPresence::required),
                field(
                    &DepositDepthInterval::m_max, schema::metadata("max"), FloatDomain{0.0, 1.0}, EPresence::required)};
        }
    };

    struct RadiusInterval {
        double m_min_m{2.0}, m_max_m{3.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&RadiusInterval::m_min_m, schema::metadata("min_m"), FloatDomain{0.0, 3.0}),
                field(&RadiusInterval::m_max_m, schema::metadata("max_m"), FloatDomain{0.0, 3.0})};
        }
    };

    struct WeightedMaterial {
        MaterialReference m_material{};
        double m_weight{1.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &WeightedMaterial::m_material, schema::metadata("material"), choice_reference, EPresence::required),
                field(
                    &WeightedMaterial::m_weight, schema::metadata("weight"),
                    FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}, EPresence::required)};
        }
    };

    struct CompositionBand {
        u32 m_order{};
        DepthInterval m_depth{};
        FieldReference m_field{};
        KeyedCollection<WeightedMaterial> m_materials{};
        double m_bed_period_m{}, m_bed_warp_m{}, m_bed_dip{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&CompositionBand::m_order, schema::metadata("order"), Range{0u, 65535u}, EPresence::required),
                group(&CompositionBand::m_depth, schema::metadata("depth"), EPresence::required),
                field(&CompositionBand::m_field, schema::metadata("field"), noise_reference, EPresence::required),
                schema::collection(&CompositionBand::m_materials, "materials", 1u, 64u),
                field(&CompositionBand::m_bed_period_m, schema::metadata("bed_period_m"), FloatDomain{0.0, 256.0}),
                field(&CompositionBand::m_bed_warp_m, schema::metadata("bed_warp_m"), FloatDomain{0.0, 64.0}),
                field(&CompositionBand::m_bed_dip, schema::metadata("bed_dip"), FloatDomain{-0.5, 0.5})};
        }
    };

    struct BlobRadius {
        double m_min_m{}, m_max_m{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&BlobRadius::m_min_m, schema::metadata("min_m"), FloatDomain{0.0, 256.0}),
                field(&BlobRadius::m_max_m, schema::metadata("max_m"), FloatDomain{0.0, 256.0})};
        }
    };

    struct BlobStretch {
        double m_min{1.0}, m_max{1.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&BlobStretch::m_min, schema::metadata("min"), FloatDomain{0.1, 16.0}),
                field(&BlobStretch::m_max, schema::metadata("max"), FloatDomain{0.1, 16.0})};
        }
    };

    struct BlobOrientation {
        double m_min_degrees{}, m_max_degrees{360.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&BlobOrientation::m_min_degrees, schema::metadata("min_degrees"), FloatDomain{0.0, 360.0}),
                field(&BlobOrientation::m_max_degrees, schema::metadata("max_degrees"), FloatDomain{0.0, 360.0})};
        }
    };

    struct DepositBlob {
        u32 m_count{};
        BlobRadius m_radius{};
        BlobStretch m_stretch{};
        BlobOrientation m_orientation{};
        double m_distortion{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&DepositBlob::m_count, schema::metadata("count"), Range{1u, 4096u}, EPresence::required),
                group(&DepositBlob::m_radius, schema::metadata("radius"), EPresence::defaulted),
                group(&DepositBlob::m_stretch, schema::metadata("stretch"), EPresence::defaulted),
                group(&DepositBlob::m_orientation, schema::metadata("orientation"), EPresence::defaulted),
                field(&DepositBlob::m_distortion, schema::metadata("distortion"), FloatDomain{0.0, 0.45})};
        }
    };

    struct Deposit {
        u32 m_order{};
        FieldReference m_field{};
        MaterialReference m_material{}, m_host_material{};
        DepositDepthInterval m_depth{};
        ThresholdInterval m_threshold{};
        EGenerationDepositStage m_stage{EGenerationDepositStage::bulk};
        double m_core_coverage{1.0}, m_fringe_coverage{1.0}, m_sprinkle_probability{};
        std::optional<DepositBlob> m_blob{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Deposit::m_order, schema::metadata("order"), Range{0u, 65535u}, EPresence::required),
                field(&Deposit::m_field, schema::metadata("field"), noise_reference, EPresence::required),
                field(&Deposit::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(
                    &Deposit::m_host_material, schema::metadata("host_material"), terrain_reference,
                    EPresence::required),
                group(&Deposit::m_depth, schema::metadata("depth"), EPresence::required),
                group(&Deposit::m_threshold, schema::metadata("threshold"), EPresence::required),
                field(
                    &Deposit::m_stage, schema::metadata("stage"),
                    config::EnumDomain<EGenerationDepositStage, 2>{
                        {config::EnumEntry{EGenerationDepositStage::bulk, "bulk"},
                            config::EnumEntry{EGenerationDepositStage::detail, "detail"}}}),
                field(&Deposit::m_core_coverage, schema::metadata("core_coverage"), FloatDomain{0.0, 1.0}),
                field(&Deposit::m_fringe_coverage, schema::metadata("fringe_coverage"), FloatDomain{0.0, 1.0}),
                field(
                    &Deposit::m_sprinkle_probability, schema::metadata("sprinkle_probability"), FloatDomain{0.0, 0.02}),
                group(&Deposit::m_blob, schema::metadata("blob"), EPresence::optional)};
        }
    };

    struct Pore {
        FieldReference m_field{};
        double m_probability{};
        RadiusInterval m_radius{};
        double m_affinity_min{};
        u32 m_passes{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Pore::m_field, schema::metadata("field"), noise_reference, EPresence::required),
                field(&Pore::m_probability, schema::metadata("probability"), FloatDomain{0.0, 0.2}),
                group(&Pore::m_radius, schema::metadata("radius"), EPresence::defaulted),
                field(&Pore::m_affinity_min, schema::metadata("affinity_min"), FloatDomain{-1.0, 1.0}),
                field(&Pore::m_passes, schema::metadata("passes"), Range{0u, 6u})};
        }
    };

    struct Lining {
        MaterialReference m_material{};
        u32 m_thickness_fine{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Lining::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(
                    &Lining::m_thickness_fine, schema::metadata("thickness_fine"), Range{1u, 3u}, EPresence::required)};
        }
    };

    struct Relaxation {
        u32 m_passes{};
        double m_adoption{0.5};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Relaxation::m_passes, schema::metadata("passes"), Range{0u, 24u}),
                field(&Relaxation::m_adoption, schema::metadata("adoption"), FloatDomain{0.0, 1.0})};
        }
    };

    struct Sprinkle {
        double m_probability{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(&Sprinkle::m_probability, schema::metadata("probability"), FloatDomain{0.0, 1.0})};
        }
    };

    struct Finish {
        u32 m_passes{};
        double m_adoption{0.15};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Finish::m_passes, schema::metadata("passes"), Range{0u, 4u}),
                field(&Finish::m_adoption, schema::metadata("adoption"), FloatDomain{0.0, 0.3})};
        }
    };

    struct Erosion {
        u32 m_passes{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{field(&Erosion::m_passes, schema::metadata("passes"), Range{0u, 24u})};
        }
    };

    struct Soil {
        MaterialReference m_material{};
        double m_depth_m{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Soil::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(&Soil::m_depth_m, schema::metadata("depth_m"), FloatDomain{0.0, 12.0})};
        }
    };

    struct Weathered {
        MaterialReference m_material{};
        double m_depth_m{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Weathered::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(&Weathered::m_depth_m, schema::metadata("depth_m"), FloatDomain{0.0, 30.0})};
        }
    };

    struct SurfaceLayers {
        std::optional<Soil> m_soil{};
        std::optional<Weathered> m_weathered{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                group(&SurfaceLayers::m_soil, schema::metadata("soil"), EPresence::optional),
                group(&SurfaceLayers::m_weathered, schema::metadata("weathered"), EPresence::optional)};
        }
    };

    struct Formation {
        MaterialReference m_material{};
        double m_probability{}, m_length_m{3.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Formation::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(&Formation::m_probability, schema::metadata("probability"), FloatDomain{0.0, 0.2}),
                field(&Formation::m_length_m, schema::metadata("length_m"), FloatDomain{0.0, 8.0})};
        }
    };

    struct Rind {
        MaterialReference m_material{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Rind::m_material, schema::metadata("material"), terrain_reference, EPresence::required)};
        }
    };

    // ------------------------------------------------------------------
    // biomes
    // ------------------------------------------------------------------

    struct Biome {
        Temperature m_temperature{};
        KeyedCollection<CompositionBand> m_composition{};
        KeyedCollection<Deposit> m_deposits{};
        std::optional<Pore> m_pore{};
        std::optional<Lining> m_lining{};
        Relaxation m_relaxation{};
        Sprinkle m_sprinkle{};
        Finish m_finish{};
        Erosion m_erosion{};
        SurfaceLayers m_surface_layers{};
        std::optional<Formation> m_formation{};
        std::optional<Rind> m_rind{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                group(&Biome::m_temperature, schema::metadata("temperature"), EPresence::required),
                schema::collection(&Biome::m_composition, "composition", 1u, 16u),
                schema::collection(&Biome::m_deposits, "deposits", 0u, 64u, EPresence::defaulted),
                group(&Biome::m_pore, schema::metadata("pore"), EPresence::optional),
                group(&Biome::m_lining, schema::metadata("lining"), EPresence::optional),
                group(&Biome::m_relaxation, schema::metadata("relaxation"), EPresence::defaulted),
                group(&Biome::m_sprinkle, schema::metadata("sprinkle"), EPresence::defaulted),
                group(&Biome::m_finish, schema::metadata("finish"), EPresence::defaulted),
                group(&Biome::m_erosion, schema::metadata("erosion"), EPresence::defaulted),
                group(&Biome::m_surface_layers, schema::metadata("surface_layers"), EPresence::defaulted),
                group(&Biome::m_formation, schema::metadata("formation"), EPresence::optional),
                group(&Biome::m_rind, schema::metadata("rind"), EPresence::optional)};
        }
    };

    // ------------------------------------------------------------------
    // geometry and operations
    // ------------------------------------------------------------------

    struct Rectangle {
        u32 m_width_fine{}, m_height_fine{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Rectangle::m_width_fine, schema::metadata("width_fine"), Range{1u, 4096u}, EPresence::required),
                field(
                    &Rectangle::m_height_fine, schema::metadata("height_fine"), Range{1u, 4096u}, EPresence::required)};
        }
    };

    struct Ellipse {
        u32 m_width_fine{}, m_height_fine{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Ellipse::m_width_fine, schema::metadata("width_fine"), Range{1u, 4096u}, EPresence::required),
                field(&Ellipse::m_height_fine, schema::metadata("height_fine"), Range{1u, 4096u}, EPresence::required)};
        }
    };

    struct Capsule {
        i32 m_end_x_fine{}, m_end_y_fine{};
        double m_radius_m{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &Capsule::m_end_x_fine, schema::metadata("end_x_fine"), Range<i32>{-4096, 4096},
                    EPresence::required),
                field(
                    &Capsule::m_end_y_fine, schema::metadata("end_y_fine"), Range<i32>{-4096, 4096},
                    EPresence::required),
                field(
                    &Capsule::m_radius_m, schema::metadata("radius_m"), FloatDomain{0.0, 4096.0 / 3.0, false, true},
                    EPresence::required)};
        }
    };

    using ShapeVariant = std::variant<Rectangle, Ellipse, Capsule>;

    struct Paint {
        MaterialReference m_material{};
        ShapeVariant m_shape{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Paint::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                config::taggedVariant(
                    &Paint::m_shape, schema::metadata("shape"), EPresence::required,
                    config::Alternative<Rectangle>{"rectangle"}, config::Alternative<Ellipse>{"ellipse"},
                    config::Alternative<Capsule>{"capsule"})};
        }
    };

    struct Carve {
        ShapeVariant m_shape{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{config::taggedVariant(
                &Carve::m_shape, schema::metadata("shape"), EPresence::required,
                config::Alternative<Rectangle>{"rectangle"}, config::Alternative<Ellipse>{"ellipse"},
                config::Alternative<Capsule>{"capsule"})};
        }
    };

    using ActionVariant = std::variant<Paint, Carve>;

    // Semantic keys live in KeyedEntry, as for the other canonical collections.
    struct Operation {
        u32 m_order{};
        i32 m_x_fine{}, m_y_fine{};
        ActionVariant m_action{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Operation::m_order, schema::metadata("order"), Range{0u, 65535u}, EPresence::required),
                field(&Operation::m_x_fine, schema::metadata("x_fine"), Range<i32>{-4096, 4096}, EPresence::required),
                field(&Operation::m_y_fine, schema::metadata("y_fine"), Range<i32>{-4096, 4096}, EPresence::required),
                config::taggedVariant(
                    &Operation::m_action, schema::metadata("action"), EPresence::required,
                    config::Alternative<Paint>{"paint"}, config::Alternative<Carve>{"carve"})};
        }
    };

    // ------------------------------------------------------------------
    // features
    // ------------------------------------------------------------------

    struct Marker {
        EGenerationMarker m_role{};
        i32 m_x_fine{}, m_y_fine{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &Marker::m_role, schema::metadata("role"),
                    config::EnumDomain<EGenerationMarker, 5>{
                        {config::EnumEntry{EGenerationMarker::start, "start"},
                            config::EnumEntry{EGenerationMarker::traversal_room, "traversal_room"},
                            config::EnumEntry{EGenerationMarker::sealed_pocket, "sealed_pocket"},
                            config::EnumEntry{EGenerationMarker::resource_anchor, "resource_anchor"},
                            config::EnumEntry{EGenerationMarker::inert, "inert"}}},
                    EPresence::required),
                field(&Marker::m_x_fine, schema::metadata("x_fine"), Range<i32>{-4096, 4096}, EPresence::required),
                field(&Marker::m_y_fine, schema::metadata("y_fine"), Range<i32>{-4096, 4096}, EPresence::required)};
        }
    };

    struct ReservationBounds {
        i32 m_min_x_fine{}, m_min_y_fine{}, m_max_x_fine{}, m_max_y_fine{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &ReservationBounds::m_min_x_fine, schema::metadata("min_x_fine"), Range<i32>{-4096, 4096},
                    EPresence::required),
                field(
                    &ReservationBounds::m_min_y_fine, schema::metadata("min_y_fine"), Range<i32>{-4096, 4096},
                    EPresence::required),
                field(
                    &ReservationBounds::m_max_x_fine, schema::metadata("max_x_fine"), Range<i32>{-4096, 4096},
                    EPresence::required),
                field(
                    &ReservationBounds::m_max_y_fine, schema::metadata("max_y_fine"), Range<i32>{-4096, 4096},
                    EPresence::required)};
        }
    };

    struct FeatureDefinition {
        ReservationBounds m_reservation{};
        bool m_allow_horizontal_reflection{};
        KeyedCollection<Operation> m_operations{};
        KeyedCollection<Marker> m_markers{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                group(&FeatureDefinition::m_reservation, schema::metadata("reservation"), EPresence::required),
                field(
                    &FeatureDefinition::m_allow_horizontal_reflection, schema::metadata("allow_horizontal_reflection"),
                    config::boolean_domain, EPresence::required),
                schema::collection(&FeatureDefinition::m_operations, "operations", 1u, 128u),
                schema::collection(&FeatureDefinition::m_markers, "markers", 0u, 64u, EPresence::defaulted)};
        }
    };

    struct Cave {
        BiomeReference m_biome{};
        FieldReference m_field{};
        DepositDepthInterval m_depth{};
        ThresholdInterval m_threshold{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Cave::m_biome, schema::metadata("biome"), biome_reference, EPresence::required),
                field(&Cave::m_field, schema::metadata("field"), noise_reference, EPresence::required),
                group(&Cave::m_depth, schema::metadata("depth"), EPresence::required),
                group(&Cave::m_threshold, schema::metadata("threshold"), EPresence::required)};
        }
    };

    struct WormGeometry {
        BiomeReference m_biome{};
        u32 m_count{};
        DepositDepthInterval m_depth{};
        double m_length_min_m{}, m_length_max_m{}, m_radius_min_m{}, m_radius_max_m{}, m_turn_probability{};
        double m_turn_smoothing{}, m_radius_variation{}, m_chamber_probability{}, m_chamber_scale{1.0};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&WormGeometry::m_biome, schema::metadata("biome"), biome_reference, EPresence::required),
                field(&WormGeometry::m_count, schema::metadata("count"), Range{1u, 4096u}, EPresence::required),
                group(&WormGeometry::m_depth, schema::metadata("depth"), EPresence::required),
                field(
                    &WormGeometry::m_length_min_m, schema::metadata("length_min_m"),
                    FloatDomain{0.0, 4096.0, false, true}, EPresence::required),
                field(
                    &WormGeometry::m_length_max_m, schema::metadata("length_max_m"),
                    FloatDomain{0.0, 4096.0, false, true}, EPresence::required),
                field(
                    &WormGeometry::m_radius_min_m, schema::metadata("radius_min_m"),
                    FloatDomain{0.0, 256.0, false, true}, EPresence::required),
                field(
                    &WormGeometry::m_radius_max_m, schema::metadata("radius_max_m"),
                    FloatDomain{0.0, 256.0, false, true}, EPresence::required),
                field(
                    &WormGeometry::m_turn_probability, schema::metadata("turn_probability"), FloatDomain{0.0, 1.0},
                    EPresence::required),
                field(&WormGeometry::m_turn_smoothing, schema::metadata("turn_smoothing"), FloatDomain{0.0, 1.0}),
                field(&WormGeometry::m_radius_variation, schema::metadata("radius_variation"), FloatDomain{0.0, 1.0}),
                field(
                    &WormGeometry::m_chamber_probability, schema::metadata("chamber_probability"),
                    FloatDomain{0.0, 0.1}),
                field(&WormGeometry::m_chamber_scale, schema::metadata("chamber_scale"), FloatDomain{1.0, 4.0})};
        }
    };

    struct CarveWorm {
        WormGeometry m_geometry{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{group(&CarveWorm::m_geometry, schema::metadata("geometry"), EPresence::required)};
        }
    };

    struct VeinWorm {
        WormGeometry m_geometry{};
        MaterialReference m_material{}, m_host_material{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                group(&VeinWorm::m_geometry, schema::metadata("geometry"), EPresence::required),
                field(&VeinWorm::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(
                    &VeinWorm::m_host_material, schema::metadata("host_material"), terrain_reference,
                    EPresence::required)};
        }
    };

    struct Connection {
        double m_radius_m{};
        u32 m_extra_links{}, m_natural_anchors{}, m_wells{}, m_branches{};
        double m_roughness_m{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &Connection::m_radius_m, schema::metadata("radius_m"), FloatDomain{0.0, 256.0, false, true},
                    EPresence::required),
                field(&Connection::m_extra_links, schema::metadata("extra_links"), Range{0u, 1024u}),
                field(&Connection::m_natural_anchors, schema::metadata("natural_anchors"), Range{0u, 128u}),
                field(&Connection::m_wells, schema::metadata("wells"), Range{0u, 32u}),
                field(&Connection::m_branches, schema::metadata("branches"), Range{0u, 64u}),
                field(&Connection::m_roughness_m, schema::metadata("roughness_m"), FloatDomain{0.0, 8.0})};
        }
    };

    struct Feature {
        FeatureReference m_feature{};
        BiomeReference m_biome{};
        DepositDepthInterval m_depth{};
        double m_spacing_m{};
        u32 m_min_count{}, m_max_count{}, m_attempts{}, m_priority{};
        bool m_reflect{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Feature::m_feature, schema::metadata("feature"), feature_reference, EPresence::required),
                field(&Feature::m_biome, schema::metadata("biome"), biome_reference, EPresence::required),
                group(&Feature::m_depth, schema::metadata("depth"), EPresence::required),
                field(
                    &Feature::m_spacing_m, schema::metadata("spacing_m"), FloatDomain{0.0, 4096.0},
                    EPresence::required),
                field(&Feature::m_min_count, schema::metadata("min_count"), Range{0u, 1024u}, EPresence::required),
                field(&Feature::m_max_count, schema::metadata("max_count"), Range{0u, 1024u}, EPresence::required),
                field(&Feature::m_attempts, schema::metadata("attempts"), Range{1u, 4096u}, EPresence::required),
                field(&Feature::m_priority, schema::metadata("priority"), Range{0u, 65535u}),
                field(&Feature::m_reflect, schema::metadata("reflect"), config::boolean_domain)};
        }
    };
    // ------------------------------------------------------------------
    // matter
    // ------------------------------------------------------------------

    struct MatterFill {
        RuleReference m_placement{};
        MarkerReference m_marker{};
        MaterialReference m_material{};
        double m_temperature_k{}, m_bulk_density_kg_m3{};
        std::optional<double> m_fill_height_m{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &MatterFill::m_placement, schema::metadata("placement"), placement_reference, EPresence::required),
                field(&MatterFill::m_marker, schema::metadata("marker"), pocket_marker_reference, EPresence::required),
                field(&MatterFill::m_material, schema::metadata("material"), particle_reference, EPresence::required),
                field(
                    &MatterFill::m_temperature_k, schema::metadata("temperature_k"),
                    FloatDomain{0.0, static_cast<double>((std::numeric_limits<float>::max)()), false, true},
                    EPresence::required),
                field(
                    &MatterFill::m_bulk_density_kg_m3, schema::metadata("bulk_density_kg_m3"),
                    FloatDomain{0.0, 1.0e9, false, true}, EPresence::required),
                field(
                    &MatterFill::m_fill_height_m, schema::metadata("fill_height_m"), FloatDomain{-4096.0, 4096.0},
                    EPresence::optional)};
        }
    };
    using RuleVariant = std::variant<Cave, CarveWorm, VeinWorm, Connection, Feature, MatterFill>;
    [[nodiscard]] constexpr EGenerationPhase phaseOf(const Cave &) noexcept {
        return EGenerationPhase::carve;
    }
    [[nodiscard]] constexpr EGenerationPhase phaseOf(const CarveWorm &) noexcept {
        return EGenerationPhase::carve;
    }
    [[nodiscard]] constexpr EGenerationPhase phaseOf(const VeinWorm &) noexcept {
        return EGenerationPhase::deposit;
    }
    [[nodiscard]] constexpr EGenerationPhase phaseOf(const Connection &) noexcept {
        return EGenerationPhase::carve;
    }
    [[nodiscard]] constexpr EGenerationPhase phaseOf(const Feature &) noexcept {
        return EGenerationPhase::stamp;
    }
    [[nodiscard]] constexpr EGenerationPhase phaseOf(const MatterFill &) noexcept {
        return EGenerationPhase::fill;
    }
    [[nodiscard]] constexpr EGenerationPhase phaseOf(const RuleVariant &payload) {
        return std::visit([](const auto &value) { return phaseOf(value); }, payload);
    }
    // ------------------------------------------------------------------
    // rules and layout
    // ------------------------------------------------------------------

    struct RuleDefinition {
        u32 m_order{};
        bool m_enabled{true};
        RuleVariant m_payload{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&RuleDefinition::m_order, schema::metadata("order"), Range{0u, 65535u}, EPresence::required),
                field(&RuleDefinition::m_enabled, schema::metadata("enabled"), config::boolean_domain),
                config::taggedVariant(
                    &RuleDefinition::m_payload, schema::metadata("payload"), EPresence::required,
                    config::Alternative<Cave>{"cave"}, config::Alternative<CarveWorm>{"carve_worm"},
                    config::Alternative<VeinWorm>{"vein_worm"}, config::Alternative<Connection>{"connection"},
                    config::Alternative<Feature>{"feature"}, config::Alternative<MatterFill>{"matter_fill"})};
        }
    };

    struct Layout {
        u32 m_min_sites{}, m_max_sites{}, m_site_attempts{};
        double m_site_spacing_m{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &Layout::m_min_sites, schema::metadata("min_sites"), Range{1u, kMaxGenerationSites},
                    EPresence::required),
                field(
                    &Layout::m_max_sites, schema::metadata("max_sites"), Range{1u, kMaxGenerationSites},
                    EPresence::required),
                field(
                    &Layout::m_site_attempts, schema::metadata("site_attempts"), Range{1u, 4096u}, EPresence::required),
                field(
                    &Layout::m_site_spacing_m, schema::metadata("site_spacing_m"),
                    FloatDomain{0.0, 4096.0, false, true}, EPresence::required)};
        }
    };

    struct Boundaries {
        MaterialReference m_material{};
        u32 m_thickness_fine{};
        double m_warp_amplitude_m{};
        FieldReference m_warp_x_field{}, m_warp_y_field{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Boundaries::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(
                    &Boundaries::m_thickness_fine, schema::metadata("thickness_fine"), Range{1u, 2047u},
                    EPresence::required),
                field(&Boundaries::m_warp_amplitude_m, schema::metadata("warp_amplitude_m"), FloatDomain{0.0, 4096.0}),
                field(
                    &Boundaries::m_warp_x_field, schema::metadata("warp_x_field"), noise_reference,
                    EPresence::required),
                field(
                    &Boundaries::m_warp_y_field, schema::metadata("warp_y_field"), noise_reference,
                    EPresence::required)};
        }
    };

    struct SurfaceBedrock {
        FieldReference m_surface_field{}, m_bedrock_field{};
        MaterialReference m_bedrock_material{};
        double m_surface_elevation_m{}, m_surface_amplitude_m{}, m_bedrock_thickness_m{}, m_bedrock_variation_m{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &SurfaceBedrock::m_surface_field, schema::metadata("surface_field"), noise_reference,
                    EPresence::required),
                field(
                    &SurfaceBedrock::m_bedrock_field, schema::metadata("bedrock_field"), noise_reference,
                    EPresence::required),
                field(
                    &SurfaceBedrock::m_bedrock_material, schema::metadata("bedrock_material"), terrain_reference,
                    EPresence::required),
                field(
                    &SurfaceBedrock::m_surface_elevation_m, schema::metadata("surface_elevation_m"),
                    FloatDomain{0.0, 4096.0}, EPresence::required),
                field(
                    &SurfaceBedrock::m_surface_amplitude_m, schema::metadata("surface_amplitude_m"),
                    FloatDomain{0.0, 4096.0}),
                field(
                    &SurfaceBedrock::m_bedrock_thickness_m, schema::metadata("bedrock_thickness_m"),
                    FloatDomain{0.0, 4096.0, false, true}, EPresence::required),
                field(
                    &SurfaceBedrock::m_bedrock_variation_m, schema::metadata("bedrock_variation_m"),
                    FloatDomain{0.0, 4096.0})};
        }
    };
    // ------------------------------------------------------------------
    // start and resources
    // ------------------------------------------------------------------

    struct Start {
        BiomeReference m_biome{};
        FeatureReference m_feature{};
        MarkerReference m_marker{};
        u32 m_x_fine{}, m_y_fine{};
        double m_clearance_m{}, m_route_width_m{};
        std::optional<double> m_depth_m{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Start::m_biome, schema::metadata("biome"), biome_reference, EPresence::required),
                field(&Start::m_feature, schema::metadata("feature"), feature_reference, EPresence::required),
                field(&Start::m_marker, schema::metadata("marker"), start_marker_reference, EPresence::required),
                field(&Start::m_x_fine, schema::metadata("x_fine"), Range{0u, 4095u}, EPresence::required),
                field(&Start::m_y_fine, schema::metadata("y_fine"), Range{0u, 4095u}, EPresence::required),
                field(
                    &Start::m_clearance_m, schema::metadata("clearance_m"), FloatDomain{0.0, 4096.0, false, true},
                    EPresence::required),
                field(
                    &Start::m_route_width_m, schema::metadata("route_width_m"), FloatDomain{0.0, 4096.0, false, true},
                    EPresence::required),
                field(&Start::m_depth_m, schema::metadata("depth_m"), FloatDomain{0.0, 4096.0}, EPresence::optional)};
        }
    };

    struct Resource {
        MarkerReference m_marker{};
        MaterialReference m_material{};
        u32 m_minimum_fine_cells{};
        double m_max_distance_m{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&Resource::m_marker, schema::metadata("marker"), resource_marker_reference, EPresence::required),
                field(&Resource::m_material, schema::metadata("material"), terrain_reference, EPresence::required),
                field(
                    &Resource::m_minimum_fine_cells, schema::metadata("minimum_fine_cells"), Range{1u, 16777216u},
                    EPresence::required),
                field(
                    &Resource::m_max_distance_m, schema::metadata("max_distance_m"),
                    FloatDomain{0.0, 8192.0, false, true}, EPresence::required)};
        }
    };

    struct BiomeChoice {
        BiomeReference m_biome{};
        u32 m_required_count{};
        double m_weight{1.0};
        DepositDepthInterval m_depth{};
        double m_distance_min_m{}, m_distance_max_m{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(&BiomeChoice::m_biome, schema::metadata("biome"), biome_reference, EPresence::required),
                field(
                    &BiomeChoice::m_required_count, schema::metadata("required_count"), Range{0u, kMaxGenerationSites}),
                field(
                    &BiomeChoice::m_weight, schema::metadata("weight"),
                    FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}, EPresence::required),
                group(&BiomeChoice::m_depth, schema::metadata("depth"), EPresence::required),
                field(
                    &BiomeChoice::m_distance_min_m, schema::metadata("distance_min_m"), FloatDomain{0.0, 8192.0},
                    EPresence::required),
                field(
                    &BiomeChoice::m_distance_max_m, schema::metadata("distance_max_m"), FloatDomain{0.0, 8192.0},
                    EPresence::required)};
        }
    };
    // ------------------------------------------------------------------
    // profiles and definition
    // ------------------------------------------------------------------

    struct ProfileDefinition {
        Layout m_layout{};
        Boundaries m_boundaries{};
        std::optional<SurfaceBedrock> m_surface_bedrock{};
        Start m_start{};
        KeyedCollection<Resource> m_resources{};
        KeyedCollection<BiomeChoice> m_biomes{};
        KeyedCollection<RuleDefinition> m_rules{};
        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                group(&ProfileDefinition::m_layout, schema::metadata("layout"), EPresence::required),
                group(&ProfileDefinition::m_boundaries, schema::metadata("boundaries"), EPresence::required),
                group(&ProfileDefinition::m_surface_bedrock, schema::metadata("surface_bedrock"), EPresence::optional),
                group(&ProfileDefinition::m_start, schema::metadata("start"), EPresence::required),
                schema::collection(&ProfileDefinition::m_resources, "resources", 0u, 8u, EPresence::defaulted),
                schema::collection(&ProfileDefinition::m_biomes, "biomes", 1u, kMaxGenerationBiomes),
                schema::collection(
                    &ProfileDefinition::m_rules, "rules", 0u, kMaxGenerationRules, EPresence::defaulted)};
        }
    };

    struct Definition {
        KeyedCollection<Material> m_materials{};
        KeyedCollection<NoiseFieldDesc> m_fields{};
        KeyedCollection<Biome> m_biomes{};
        KeyedCollection<FeatureDefinition> m_features{};
        KeyedCollection<ProfileDefinition> m_worlds{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                schema::collection(&Definition::m_materials, "materials", 1u, 256u),
                schema::collection(&Definition::m_fields, "fields", 1u, 256u),
                schema::collection(&Definition::m_biomes, "biomes", 1u, kMaxGenerationBiomes),
                schema::collection(
                    &Definition::m_features, "features", 0u, kMaxGenerationFeatures, EPresence::defaulted),
                schema::collection(&Definition::m_worlds, "worlds", 0u, 128u, EPresence::defaulted)};
        }
    };

    // ------------------------------------------------------------------
    // semantic fingerprint
    // ------------------------------------------------------------------

    // Borrows authored identities; absence and a present empty key are distinct.
    struct SemanticFingerprintScope {
        std::optional<std::string_view> m_profile{};
        std::optional<std::string_view> m_rule{};
    };

    // Borrows the canonical definition independently of surrounding pack/domain framing.
    class SemanticFingerprint {
        enum class EOwner : u8 {
            absent,
            root,
            profile,
            feature
        };

        struct Owner {
            std::string_view m_key{};
            EOwner m_kind{EOwner::absent};
        };

        struct Context {
            const ProfileDefinition *m_profile{};
            std::optional<std::string_view> m_profile_key{};
            Owner m_start{};
            Owner m_pocket{};
        };

        // Fixed canonical blocks bound encoding storage independently of input size.
        static constexpr std::size_t kBlockBytes = 512u;

        const Definition &m_definition;
        Array<std::size_t> m_ordering{};
        u64 m_hash{0x7070722e73656d33ull};
        u64 m_length{};
        std::size_t m_used{};
        std::array<u8, kBlockBytes> m_buffer{};

        void append(const void *data, std::size_t size) noexcept;
        void byte(u8 value) noexcept;

        [[nodiscard]] Context profileContext(std::string_view key) const noexcept;
        [[nodiscard]] Context ruleContext(const Context &context, std::string_view key) const noexcept;
        [[nodiscard]] Context resolveContext(const SemanticFingerprintScope &scope) const noexcept;
        [[nodiscard]] static Owner referenceOwner(
            const config::ReferenceTarget &target, const Context &context) noexcept;

        void reference(const config::ReferenceTarget &target, std::string_view key, const Context &context);

        template<typename T, typename Visitor>
        void ordered(const KeyedCollection<T> &collection, Visitor &&visitor) {
            const std::size_t frame = m_ordering.size();
            const std::size_t count = collection.m_entries.size();
            for (std::size_t i = 0u; i < count; ++i) {
                m_ordering.push_back(i);
            }

            std::sort(m_ordering.begin() + frame, m_ordering.end(), [&](const auto left, const auto right) {
                const auto &left_key = collection.m_entries[left].m_key;
                const auto &right_key = collection.m_entries[right].m_key;
                return left_key < right_key or (left_key == right_key and left < right);
            });

            for (std::size_t i = 0u; i < count; ++i) {
                // Recursion may reallocate scratch; retain offsets, never iterators.
                visitor(collection.m_entries[m_ordering[frame + i]]);
            }

            m_ordering.resize(frame);
        }

        friend u64 definitionRulesFingerprint(const Definition &definition);

        template<typename Member, typename T>
        void memberValue(const Member &member, const T &item, const Context &context) {
            using Constraint = std::remove_cvref_t<decltype(member.m_constraint)>;
            if constexpr (config::details::is_optional_v<T>) {
                byte(1u);
                byte(item.has_value() ? 1u : 0u);
                if (item) {
                    memberValue(member, *item, context);
                }

                byte(2u);
            } else if constexpr (requires { member.m_constraint.m_target; }) {
                reference(member.m_constraint.m_target, item.m_key, context);
            } else if constexpr (config::details::is_variant_domain_v<Constraint>) {
                byte(16u);
                const auto tag = config::variantTag(member, item);
                string(tag);
                // A valueless variant is an invalid draft, still deterministically encoded.
                byte(item.valueless_by_exception() ? 0u : 1u);
                if (not item.valueless_by_exception()) {
                    std::visit([&](const auto &payload) { valueInContext(payload, context); }, item);
                }

                byte(17u);
            } else {
                valueInContext(item, context);
            }
        }

        template<typename T>
        void valueInContext(const T &item, const Context &context) {
            if constexpr (config::details::is_optional_v<T>) {
                byte(1u);
                byte(item.has_value() ? 1u : 0u);
                if (item) {
                    valueInContext(*item, context);
                }

                byte(2u);
            } else if constexpr (requires { item.m_entries; }) {
                byte(5u);
                integer(static_cast<u64>(item.m_entries.size()));
                ordered(item, [&](const auto &entry) {
                    byte(6u);
                    string(entry.m_key);
                    using Item = std::remove_cvref_t<decltype(entry.m_value)>;
                    if constexpr (std::is_same_v<Item, ProfileDefinition>) {
                        valueInContext(entry.m_value, profileContext(entry.m_key));
                    } else if constexpr (std::is_same_v<Item, RuleDefinition>) {
                        valueInContext(entry.m_value, ruleContext(context, entry.m_key));
                    } else {
                        valueInContext(entry.m_value, context);
                    }

                    byte(7u);
                });

                byte(8u);
            } else if constexpr (config::details::TDescribed<T>) {
                byte(9u);
                config::visit(item, [&](const auto &member, const auto &member_value) {
                    string(member.m_metadata.m_name);
                    memberValue(member, member_value, context);
                });

                byte(10u);
            } else if constexpr (std::is_floating_point_v<T>) {
                byte(11u);
                byte(static_cast<u8>(sizeof(T)));
                const T normalized = item == T{} ? T{} : item;
                if constexpr (sizeof(T) == sizeof(u32)) {
                    integer(std::bit_cast<u32>(normalized));
                } else if constexpr (sizeof(T) == sizeof(u64)) {
                    integer(std::bit_cast<u64>(normalized));
                } else {
                    static_assert(sizeof(T) == 0u, "Unsupported floating-point identity width");
                }
            } else if constexpr (std::is_same_v<T, bool>) {
                byte(12u);
                byte(item ? 1u : 0u);
            } else if constexpr (std::is_enum_v<T>) {
                byte(13u);
                byte(static_cast<u8>(sizeof(T)));
                integer(static_cast<std::underlying_type_t<T>>(item));
            } else if constexpr (std::is_integral_v<T>) {
                byte(14u);
                byte(static_cast<u8>(sizeof(T)));
                byte(std::is_signed_v<T> ? 1u : 0u);
                integer(item);
            } else if constexpr (std::is_same_v<T, std::string> or std::is_same_v<T, std::string_view>) {
                byte(15u);
                string(item);
            } else {
                static_assert(sizeof(T) == 0u, "Semantic identity requires a described value or a supported scalar");
            }
        }

    public:
        explicit SemanticFingerprint(const Definition &definition) noexcept;

        void string(std::string_view item) noexcept;

        // Raw little-endian encoding primitive; value() supplies scalar type framing.
        template<typename T>
            requires (std::is_integral_v<T> and not std::is_same_v<T, bool>)
        void integer(const T item) noexcept {
            using U = std::make_unsigned_t<T>;
            const U bits = static_cast<U>(item);
            std::array<u8, sizeof(U)> encoded{};
            for (std::size_t i = 0u; i < sizeof(U); ++i) {
                encoded[i] = static_cast<u8>(bits >> (8u * i));
            }

            append(encoded.data(), encoded.size());
        }

        template<typename T>
        void value(const T &item, const SemanticFingerprintScope scope = {}) {
            valueInContext(item, resolveContext(scope));
        }

        [[nodiscard]] u64 hash() const noexcept;
    };

    // ------------------------------------------------------------------
    // bindings
    // ------------------------------------------------------------------

    struct MaterialBinding {
        std::string m_key{};
        u16 m_id{};
    };

    struct FieldBinding {
        std::string m_key{};
        u32 m_index{};
    };

    struct BiomeBinding {
        std::string m_key{};
        u32 m_index{};
    };

    struct MarkerBinding {
        std::string m_key{};
        u32 m_index{};
        std::string m_feature{};
    };

    struct FeatureBinding {
        std::string m_key{};
        u32 m_index{};
        Array<MarkerBinding> m_markers{};
    };

    struct RuleBinding {
        std::string m_key{};
        u32 m_index{};
        std::string m_profile{};
    };

    struct DefinitionBindings {
        // Authored entries only; vacuum is the built-in material ID zero, never authored storage.
        Array<MaterialBinding> m_materials{};
        Array<FieldBinding> m_fields{};
        Array<BiomeBinding> m_biomes{};
        Array<FeatureBinding> m_features{};
        Array<RuleBinding> m_rules{};
    };

    // ------------------------------------------------------------------
    // compilation
    // ------------------------------------------------------------------

    struct CompiledDefinition {
        Array<GenerationMaterial> m_materials{};
        Array<GenerationField> m_fields{};
        Array<GenerationBiome> m_biomes{};
        Array<GenerationFeature> m_features{};
        DefinitionBindings m_bindings{};
    };

    struct GenerationCompileParameters {
        double m_cell_size_m{}, m_reference_thickness_m{}, m_actor_half_width_m{}, m_actor_half_height_m{};
    };

    struct CompiledRecipe {
        GenerationRecipe m_recipe{};
        DefinitionBindings m_bindings{};
    };
    [[nodiscard]] Expected<CompiledRecipe> compileRecipe(
        Definition definition, const ProfileReference &profile, GenerationCompileParameters parameters,
        Diagnostics &diagnostics);

    /// Explicit global bindings cover exactly the selected definition's catalogs.
    /// Marker and rule bindings remain compiler-derived and must not be supplied.
    [[nodiscard]] Expected<CompiledRecipe> compileRecipe(
        Definition definition, const ProfileReference &profile, GenerationCompileParameters parameters,
        const DefinitionBindings &bindings, Diagnostics &diagnostics);

    // Required/unknown authoring properties are enforced by config::resolveRecursive<Definition>.
    [[nodiscard]] std::error_code validateDefinition(const Definition &definition, Diagnostics &diagnostics);
    // A numeric fragment, NOT a validated world. Noise compilation remains in compileGenerationPlan.
    [[nodiscard]] Expected<CompiledDefinition> compileDefinition(
        Definition definition, double cell_size_m, Diagnostics &diagnostics);

    [[nodiscard]] Expected<CompiledDefinition> compileDefinition(
        Definition definition, double cell_size_m, const DefinitionBindings &bindings, Diagnostics &diagnostics);
    // Resolved fragment identity, not a full world fingerprint or a validation operation.
    [[nodiscard]] u64 definitionFingerprint(const Definition &definition);

    /// Numeric semantic identity includes the explicit global alias-to-ID/index maps.
    /// Binding vector order, derived owner maps, pack names and paths are excluded.
    [[nodiscard]] u64 definitionFingerprint(const Definition &definition, const DefinitionBindings &bindings);
    // Rule collections projected from every profile, including profiles with no rules.
    [[nodiscard]] u64 definitionRulesFingerprint(const Definition &definition);
}
