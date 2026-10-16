module;
#include "pP/Macros.h"

export module game.colony.content;

import engine.core;
import engine.asset;
import engine.sim;
import engine.image;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // materials
    // ------------------------------------------------------------------

    enum class EMaterialPhase : u8 {
        solid,
        liquid,
        gas
    };
    enum class EMaterialRepresentation : u8 {
        terrain,
        particle
    };
    enum class EMaterialAlphaMode : u8 {
        opaque,
        blend
    };
    enum class EMaterialTextureSampling : u8 {
        stochastic,
        repeat
    };

    struct MaterialDefinition {
        std::string m_key{};
        u16 m_numeric_id{0};
        std::string m_display_name{};
        EMaterialPhase m_phase{EMaterialPhase::solid};
        EMaterialRepresentation m_representation{EMaterialRepresentation::terrain};
        // Packed RRGGBBAA, authored in sRGB. Shader conversion is a presentation concern.
        u32 m_color_rgba{0};
        std::optional<std::string> m_albedo_texture{};
        std::optional<double> m_texture_period_m{};
        std::optional<bool> m_diggable{};
        double m_reference_density_kg_m3{0.0};
        // Diagnostic provenance is owned but excluded from canonical content identity.
        std::string m_albedo_source_location{};
        double m_roundness{1.0};
        double m_metallic{}, m_roughness{1.0};
        std::array<double, 3u> m_emissive_rgb{};
        double m_emissive_strength{1.0};
        EMaterialAlphaMode m_alpha_mode{EMaterialAlphaMode::opaque};
        double m_ior{1.5};
        std::optional<std::string> m_normal_texture{};
        double m_normal_scale{1.0}, m_refraction_depth_m{};
        std::string m_normal_source_location{};
        EMaterialTextureSampling m_texture_sampling{EMaterialTextureSampling::stochastic};
    };

    // ------------------------------------------------------------------
    // limits
    // ------------------------------------------------------------------

    inline constexpr std::size_t kMaxMaterialDocumentBytes = asset::kMaxDescriptorBytes;
    inline constexpr std::size_t kMaxContentMaterials = sim::definition::Definition::materials_domain.m_max;
    inline constexpr std::size_t kMaxContentFields = sim::definition::Definition::fields_domain.m_max;
    inline constexpr std::size_t kMaxContentBiomes = sim::definition::Definition::biomes_domain.m_max;
    inline constexpr std::size_t kMaxBiomeCompositionBands = 16u;
    inline constexpr std::size_t kMaxCompositionMaterials = 64u;
    inline constexpr std::size_t kMaxBiomeDeposits = 64u;
    inline constexpr std::size_t kMaxContentFeatures = sim::definition::Definition::features_domain.m_max;
    inline constexpr std::size_t kMaxFeatureOperations = 128u;
    inline constexpr std::size_t kMaxFeatureMarkers = 64u;
    inline constexpr std::size_t kMaxContentWorlds = 16u;
    inline constexpr std::size_t kMaxWorldRules = sim::definition::ProfileDefinition::rules_domain.m_max;
    inline constexpr std::size_t kMaxWorldSites = 4096u;
    inline constexpr std::size_t kMaxStartResources = 8u;
    inline constexpr std::size_t kMaxPackManifestBytes = asset::kMaxManifestBytes;

    // ------------------------------------------------------------------
    // documents
    // ------------------------------------------------------------------

    enum class EContentDocument : u8 {
        materials,
        biomes,
        fields,
        features,
        worlds,
        rules,
        elements,
        textures,
        backgrounds,
        count
    };
    inline constexpr std::size_t kContentDocumentCount = static_cast<std::size_t>(EContentDocument::count);

    struct ContentDocumentBinding {
        EContentDocument m_document{};
        std::string_view m_collection{}, m_label{};
    };
    [[nodiscard]] std::span<const ContentDocumentBinding> contentDocumentBindings() noexcept;
    [[nodiscard]] std::string_view contentAssetKind(EContentDocument category) noexcept;
    /// Numeric limits and enum choices come from the decoding schemas. Slider
    /// bounds are viewport hints only; editors must include the current value.
    /// Choice lists are owned; their string views have immutable static lifetime.
    struct ContentPropertyDescriptor : config::PropertyDescriptor {
        std::optional<std::size_t> m_text_length{};
        std::optional<EContentDocument> m_reference_category{};
    };
    [[nodiscard]] std::optional<ContentPropertyDescriptor> contentPropertyDescriptor(
        EContentDocument category, std::string_view relative_property, const config::Document &document,
        std::string_view definition_key);
    // ------------------------------------------------------------------
    // complete owned multi-pack descriptor sources
    // ------------------------------------------------------------------

    struct ContentSourceLimits {
        /// Logical source text per complete candidate, excluding history and
        /// transient copies. Individual files retain foundation byte limits.
        static constexpr std::size_t maximum_text_bytes = 7u * asset::kMaxDescriptorBytes + asset::kMaxManifestBytes;
    };

    struct ContentSourceSet {
        Array<asset::PackSource> m_packs{};
        Array<asset::DescriptorSource> m_descriptors{};

        [[nodiscard]] Expected<asset::DescriptorSource> readDescriptor(
            const asset::IndexedAsset &entry, config::Diagnostics &diagnostics) const;
        [[nodiscard]] bool sameMounts(const ContentSourceSet &other) const noexcept;
        [[nodiscard]] bool sameMounts(const asset::PackSet &other) const noexcept;
    };

    /// Matching owned text and immutable index for one candidate. Editing starts
    /// from a separate SourceSet value and requires an explicit new snapshot.
    class ContentSourceSnapshot {
    public:
        class Restoration {
            asset::QualifiedName m_name;
            std::string m_kind;
            std::filesystem::path m_root, m_path;
            bool m_valid{true};

            Restoration(
                asset::QualifiedName name, std::string kind, std::filesystem::path root, std::filesystem::path path);
            friend class ContentSourceSnapshot;

        public:
            Restoration(const Restoration &) = default;
            Restoration &operator=(const Restoration &) = default;

            Restoration(Restoration &&other) noexcept;
            Restoration &operator=(Restoration &&other) noexcept;

            [[nodiscard]] const asset::QualifiedName &name() const noexcept;
            [[nodiscard]] std::string_view kind() const noexcept;
            [[nodiscard]] const std::filesystem::path &root() const noexcept;
            [[nodiscard]] const std::filesystem::path &path() const noexcept;
            [[nodiscard]] bool matches(const asset::PackSet &packs, const asset::IndexedAsset &entry) const noexcept;
        };

    private:
        ContentSourceSet m_source;
        std::shared_ptr<const asset::PackSet> m_index;

        ContentSourceSnapshot(ContentSourceSet source, std::shared_ptr<const asset::PackSet> index);
        friend Expected<ContentSourceSnapshot> loadContentSourceSet(
            std::span<const std::filesystem::path>, config::Diagnostics &, const IContext *);

    public:
        ContentSourceSnapshot(ContentSourceSnapshot &&) noexcept = default;
        ContentSourceSnapshot &operator=(ContentSourceSnapshot &&) noexcept = default;

        ContentSourceSnapshot(const ContentSourceSnapshot &) = delete;
        ContentSourceSnapshot &operator=(const ContentSourceSnapshot &) = delete;

        [[nodiscard]] static Expected<ContentSourceSnapshot> index(
            ContentSourceSet source, config::Diagnostics &diagnostics, const IContext *context = nullptr);
        [[nodiscard]] static Expected<ContentSourceSnapshot> index(
            ContentSourceSet source, std::span<const Restoration> restorations, config::Diagnostics &diagnostics,
            const IContext *context = nullptr);
        /// Builds the manifest index once, then obtains its exact descriptor set.
        /// The required reader is synchronous and never retained or bypassed.
        [[nodiscard]] static Expected<ContentSourceSnapshot> index(
            Array<asset::PackSource> manifests, asset::DescriptorReader reader,
            std::span<const Restoration> restorations, config::Diagnostics &diagnostics,
            const IContext *context = nullptr);
        [[nodiscard]] Expected<Restoration> ownedTarget(asset::AssetKey key, config::Diagnostics &diagnostics) const;

        [[nodiscard]] const ContentSourceSet &source() const noexcept;
        [[nodiscard]] std::shared_ptr<const asset::PackSet> packOwner() const noexcept;
    };

    /// New authored targets use an injective kind directory and a device-safe
    /// lowercase filename. Existing declared paths retain their original spelling.
    [[nodiscard]] Expected<std::filesystem::path> newContentDescriptorPath(
        const asset::PackSet &packs, const asset::QualifiedName &name, std::string_view kind,
        config::Diagnostics &diagnostics);
    [[nodiscard]] Expected<ContentSourceSnapshot> loadContentSourceSet(
        std::span<const std::filesystem::path> roots, config::Diagnostics &diagnostics,
        const IContext *context = nullptr);

    // ------------------------------------------------------------------
    // fields
    // ------------------------------------------------------------------

    struct FieldDefinition {
        std::string m_key{};
        sim::NoiseFieldDesc m_noise{};
    };

    class ContentCatalog;

    // ------------------------------------------------------------------
    // biomes
    // ------------------------------------------------------------------

    struct WeightedTerrainMaterial {
        std::string m_key{};
        u16 m_material_id{};
        double m_weight{};
    };
    struct BiomeCompositionBand {
        std::string m_key{};
        u32 m_order{};
        double m_depth_min_normalized{};
        double m_depth_max_normalized{};
        std::string m_field{};
        Array<WeightedTerrainMaterial> m_materials{};
        double m_total_weight{};
        double m_bed_period_m{}, m_bed_warp_m{}, m_bed_dip{};
    };
    struct BiomeThresholdDeposit {
        std::string m_key{};
        u32 m_order{};
        std::string m_field{};
        u16 m_material_id{};
        u16 m_host_material_id{};
        double m_depth_min_normalized{};
        double m_depth_max_normalized{};
        double m_threshold_min{};
        double m_threshold_max{};
        sim::EGenerationDepositShape m_shape{sim::EGenerationDepositShape::field};
        sim::EGenerationDepositStage m_stage{sim::EGenerationDepositStage::bulk};
        u32 m_blob_count{};
        double m_radius_min_m{}, m_radius_max_m{};
        double m_stretch_min{1.0}, m_stretch_max{1.0}, m_distortion{};
        double m_core_coverage{1.0}, m_fringe_coverage{1.0}, m_sprinkle_probability{};
        double m_orientation_min_degrees{}, m_orientation_max_degrees{360.0};
    };
    struct BiomeDefinition {
        std::string m_key{};
        std::string m_display_name{};
        u32 m_preview_color_rgba{};
        double m_temperature_base_k{};
        double m_temperature_variation_k{};
        std::string m_temperature_field{};
        Array<BiomeCompositionBand> m_composition{};
        Array<BiomeThresholdDeposit> m_deposits{};
        u16 m_lining_material_id{};
        u32 m_lining_thickness_fine{};
        u32 m_relaxation_passes{};
        double m_relaxation_adoption{0.5};
        double m_sprinkle_probability{};
        std::optional<std::string> m_pore_field{};
        double m_pore_probability{};
        double m_pore_radius_min_m{2.0};
        double m_pore_radius_max_m{3.0};
        double m_pore_affinity_min{};
        u32 m_pore_passes{};
        u32 m_finish_passes{};
        double m_finish_adoption{0.15};
        u16 m_soil_material_id{};
        u16 m_weathered_material_id{};
        double m_soil_depth_m{};
        double m_weathered_depth_m{};
        u16 m_formation_material_id{};
        u16 m_rind_material_id{};
        double m_formation_probability{};
        double m_formation_length_m{3.0};
        u32 m_erosion_passes{};
    };

    // Pure, owned biome recipes with terrain references resolved to validated
    // numeric IDs. Fields retain their stable stream keys. Explicit ordered
    // composition bands cover normalized depth [0,1] without gaps/overlap.
    // ------------------------------------------------------------------
    // features
    // ------------------------------------------------------------------

    enum class EFeatureAction : u8 {
        paint,
        carve
    };
    enum class EFeatureShape : u8 {
        rectangle,
        ellipse,
        capsule
    };
    enum class EFeatureMarkerRole : u8 {
        start,
        traversal_room,
        sealed_pocket,
        resource_anchor,
        inert
    };
    struct FeatureBounds {
        i32 m_min_x_fine{};
        i32 m_min_y_fine{};
        i32 m_max_x_fine{};
        i32 m_max_y_fine{};
    };
    struct FeatureOperation {
        std::string m_key{};
        u32 m_order{};
        EFeatureAction m_action{};
        EFeatureShape m_shape{};
        i32 m_x_fine{};
        i32 m_y_fine{};
        u32 m_width_fine{};
        u32 m_height_fine{};
        i32 m_end_x_fine{};
        i32 m_end_y_fine{};
        double m_radius_m{};
        // Zero only for carve operations; paint resolves a terrain material.
        u16 m_material_id{};
    };
    struct FeatureMarker {
        std::string m_key{};
        EFeatureMarkerRole m_role{};
        i32 m_x_fine{};
        i32 m_y_fine{};
    };
    struct FeatureDefinition {
        std::string m_key{};
        std::string m_display_name{};
        bool m_allow_horizontal_reflection{};
        FeatureBounds m_reservation{};
        Array<FeatureOperation> m_operations{};
        Array<FeatureMarker> m_markers{};
    };
    // ------------------------------------------------------------------
    // worlds
    // ------------------------------------------------------------------

    struct WorldBiomeChoice {
        std::string m_key{};
        std::string m_biome{};
        double m_weight{};
        u32 m_required_count{};
        double m_depth_min_normalized{}, m_depth_max_normalized{};
        double m_distance_min_m{}, m_distance_max_m{};
    };
    struct WorldStartResource {
        std::string m_key{}, m_marker{};
        u16 m_material_id{};
        u32 m_minimum_fine_cells{};
        double m_max_distance_m{};
    };
    struct WorldProfileDefinition {
        std::string m_key{}, m_display_name{};
        u16 m_default_wall_material_id{};
        u32 m_boundary_fine{};
        u32 m_min_sites{}, m_max_sites{}, m_site_attempts{};
        double m_site_spacing_m{}, m_boundary_warp_amplitude_m{};
        std::string m_boundary_warp_x_field{}, m_boundary_warp_y_field{};
        std::string m_start_biome{}, m_start_feature{}, m_start_marker{};
        u32 m_start_x_fine{}, m_start_y_fine{};
        double m_start_clearance_m{}, m_start_route_width_m{};
        Array<WorldStartResource> m_start_resources{};
        Array<WorldBiomeChoice> m_biomes{};
        Array<std::string> m_rule_keys{};
        std::string m_background_stack{};
        std::optional<std::string> m_surface_field{}, m_bedrock_field{};
        double m_surface_elevation_m{}, m_surface_amplitude_m{}, m_bedrock_thickness_m{}, m_bedrock_variation_m{};
        u16 m_bedrock_material_id{};
        std::optional<double> m_start_depth_m{};
    };

    // ------------------------------------------------------------------
    // backgrounds
    // ------------------------------------------------------------------

    struct BackgroundLayerDefinition {
        std::string m_key{}, m_texture{}, m_source_location{};
        u32 m_order{}, m_color_rgba{};
        double m_repeat_period_m{}, m_parallax_factor{};
    };
    struct BackgroundStackDefinition {
        std::string m_key{};
        Array<BackgroundLayerDefinition> m_layers{};
    };
    // ------------------------------------------------------------------
    // textures
    // ------------------------------------------------------------------

    /// Internal flattened definition model used by typed projection. Pack
    /// manifests and individual descriptors declare their own schema versions.
    inline constexpr u32 kContentSchemaVersion = 3u;
    inline constexpr u64 kMaxTextureInputBytes = image::TexturePreparationLimits::image_input_domain.m_max;
    inline constexpr u64 kMaxPackTextureInputBytes = image::TexturePreparationLimits::total_input_domain.m_max;
    inline constexpr u64 kMaxPackTextureDecodedBytes = image::TexturePreparationLimits::total_output_domain.m_max;
    inline constexpr u32 kMaxContentTextureDimension = image::TexturePreparationLimits::dimension_domain.m_max;
    inline constexpr u32 kMaxContentTextures = image::TexturePreparationLimits::count_domain.m_max;
    struct ContentTexture {
        std::string m_path{};
        std::filesystem::path m_source_path{};
        u64 m_source_size{};
        u64 m_source_fingerprint{};
        image::ImageAsset m_image{};
        // Usage is part of asset identity, even when source bytes/path match.
        bool m_normal_map{};
    };

    namespace appearance_schema {
        constexpr config::Metadata metadata(const std::string_view name) {
            return {name, "Content appearance", "Appearance", config::EMutability::startup, config::ESource::file};
        }
        template<typename Owner, typename T>
        constexpr auto collection(T Owner::*member, const std::string_view name, const std::size_t maximum) {
            auto result = config::collection(member, metadata(name), {0u, maximum});
            result.m_presence = config::EPresence::defaulted;
            return result;
        }
    }

    struct MaterialAppearance {
        std::string m_display_name{}, m_color_rgba{};
        std::optional<std::string> m_albedo_texture{}, m_normal_texture{};
        std::optional<double> m_texture_period_m{};
        std::optional<double> m_roundness{};
        double m_metallic{}, m_roughness{1.0};
        double m_emissive_r{}, m_emissive_g{}, m_emissive_b{}, m_emissive_strength{1.0};
        double m_ior{1.5}, m_normal_scale{1.0}, m_refraction_depth_m{};
        EMaterialAlphaMode m_alpha_mode{EMaterialAlphaMode::opaque};
        EMaterialTextureSampling m_texture_sampling{EMaterialTextureSampling::stochastic};
        static constexpr auto describe() {
            using namespace config;
            using appearance_schema::metadata;
            return std::tuple{
                field(
                    &MaterialAppearance::m_display_name, metadata("display_name"), StringDomain{256u, false},
                    EPresence::required),
                field(
                    &MaterialAppearance::m_color_rgba, metadata("color_rgba"), StringDomain{8u, false},
                    EPresence::required),
                field(
                    &MaterialAppearance::m_albedo_texture, metadata("albedo_texture"),
                    asset::QualifiedName::reference_domain, EPresence::optional),
                field(
                    &MaterialAppearance::m_normal_texture, metadata("normal_texture"),
                    asset::QualifiedName::reference_domain, EPresence::optional),
                field(
                    &MaterialAppearance::m_texture_period_m, metadata("texture_period_m"),
                    FloatDomain{
                        4096.0 / static_cast<double>((std::numeric_limits<float>::max)()),
                        static_cast<double>((std::numeric_limits<float>::max)())},
                    EPresence::optional),
                field(
                    &MaterialAppearance::m_roundness, metadata("roundness"), FloatDomain{0.0, 1.0},
                    EPresence::optional),
                field(&MaterialAppearance::m_metallic, metadata("metallic"), FloatDomain{0.0, 1.0}),
                field(&MaterialAppearance::m_roughness, metadata("roughness"), FloatDomain{0.0, 1.0}),
                field(&MaterialAppearance::m_emissive_r, metadata("emissive_r"), FloatDomain{0.0, 1.0}),
                field(&MaterialAppearance::m_emissive_g, metadata("emissive_g"), FloatDomain{0.0, 1.0}),
                field(&MaterialAppearance::m_emissive_b, metadata("emissive_b"), FloatDomain{0.0, 1.0}),
                field(
                    &MaterialAppearance::m_emissive_strength, metadata("emissive_strength"),
                    FloatDomain{0.0, static_cast<double>((std::numeric_limits<float>::max)())}),
                field(&MaterialAppearance::m_ior, metadata("ior"), FloatDomain{1.0, 3.0}),
                field(&MaterialAppearance::m_normal_scale, metadata("normal_scale"), FloatDomain{0.0, 4.0}),
                field(&MaterialAppearance::m_refraction_depth_m, metadata("refraction_depth_m"), FloatDomain{0.0, 1.0}),
                field(
                    &MaterialAppearance::m_alpha_mode, metadata("alpha_mode"),
                    EnumDomain<EMaterialAlphaMode, 2>{
                        {EnumEntry{EMaterialAlphaMode::opaque, "opaque"},
                            EnumEntry{EMaterialAlphaMode::blend, "blend"}}}),
                field(
                    &MaterialAppearance::m_texture_sampling, metadata("texture_sampling"),
                    EnumDomain<EMaterialTextureSampling, 2>{
                        {EnumEntry{EMaterialTextureSampling::stochastic, "stochastic"},
                            EnumEntry{EMaterialTextureSampling::repeat, "repeat"}}})};
        }
    };

    struct BiomeAppearance {
        std::string m_display_name{}, m_preview_color_rgba{};
        static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &BiomeAppearance::m_display_name, appearance_schema::metadata("display_name"),
                    config::StringDomain{256u, false}, config::EPresence::required),
                config::field(
                    &BiomeAppearance::m_preview_color_rgba, appearance_schema::metadata("preview_color_rgba"),
                    config::StringDomain{8u, false}, config::EPresence::required)};
        }
    };

    struct FeatureAppearance {
        std::string m_display_name{};
        static constexpr auto describe() {
            return std::tuple{config::field(
                &FeatureAppearance::m_display_name, appearance_schema::metadata("display_name"),
                config::StringDomain{256u, false}, config::EPresence::required)};
        }
    };

    struct WorldAppearance {
        std::string m_display_name{}, m_background_stack{};
        static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &WorldAppearance::m_display_name, appearance_schema::metadata("display_name"),
                    config::StringDomain{256u, false}, config::EPresence::required),
                config::field(
                    &WorldAppearance::m_background_stack, appearance_schema::metadata("background_stack"),
                    asset::QualifiedName::reference_domain, config::EPresence::required)};
        }
    };

    struct BackgroundAppearanceLayer {
        u32 m_order{};
        std::string m_texture{}, m_color_rgba{};
        double m_repeat_period_m{}, m_parallax_factor{};
        static constexpr auto describe() {
            using namespace config;
            using appearance_schema::metadata;
            return std::tuple{
                field(&BackgroundAppearanceLayer::m_order, metadata("order"), Range{0u, 3u}, EPresence::required),
                field(
                    &BackgroundAppearanceLayer::m_texture, metadata("texture"), asset::QualifiedName::reference_domain,
                    EPresence::required),
                field(
                    &BackgroundAppearanceLayer::m_color_rgba, metadata("color_rgba"), StringDomain{8u, false},
                    EPresence::required),
                field(
                    &BackgroundAppearanceLayer::m_repeat_period_m, metadata("repeat_period_m"),
                    FloatDomain{0.125, 65536.0}, EPresence::required),
                field(
                    &BackgroundAppearanceLayer::m_parallax_factor, metadata("parallax_factor"), FloatDomain{0.0, 1.0},
                    EPresence::required)};
        }
    };

    struct BackgroundAppearance {
        config::KeyedCollection<BackgroundAppearanceLayer> m_layers{};
        static constexpr auto describe() {
            return std::tuple{
                config::collection(&BackgroundAppearance::m_layers, appearance_schema::metadata("layers"), {4u, 4u})};
        }
    };

    struct Appearance {
        config::KeyedCollection<MaterialAppearance> m_materials{};
        config::KeyedCollection<BiomeAppearance> m_biomes{};
        config::KeyedCollection<FeatureAppearance> m_features{};
        config::KeyedCollection<WorldAppearance> m_worlds{};
        config::KeyedCollection<BackgroundAppearance> m_backgrounds{};
        static constexpr auto describe() {
            return std::tuple{
                appearance_schema::collection(&Appearance::m_materials, "materials", kMaxContentMaterials),
                appearance_schema::collection(&Appearance::m_biomes, "biomes", kMaxContentBiomes),
                appearance_schema::collection(&Appearance::m_features, "features", kMaxContentFeatures),
                appearance_schema::collection(&Appearance::m_worlds, "worlds", kMaxContentWorlds),
                appearance_schema::collection(&Appearance::m_backgrounds, "backgrounds", kMaxContentWorlds)};
        }
    };

    // ------------------------------------------------------------------
    // named descriptor composition
    // ------------------------------------------------------------------

    PPR_DECLARE_LOG_CATEGORY(ContentDescriptors);
    PPR_DECLARE_LOG_CATEGORY(ContentAssembly);

    [[nodiscard]] Expected<u32> decodeAppearanceColor(
        std::string_view value, std::string_view source, std::string_view property, config::Diagnostics &diagnostics);

    inline constexpr std::string_view kBackgroundAssetKind = "colony.background";
    inline constexpr std::string_view kContentAssetKinds[] = {
        sim::asset::kElementKind, sim::asset::kMaterialKind, sim::asset::kFieldKind, sim::asset::kBiomeKind,
        sim::asset::kFeatureKind, sim::asset::kRuleKind, sim::asset::kWorldKind, image::kTextureAssetKind,
        kBackgroundAssetKind};

    struct MaterialAssetDescriptor : sim::asset::MaterialDescriptor {
        MaterialAppearance m_appearance{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple_cat(
                sim::asset::MaterialDescriptor::describe(),
                std::tuple{config::group(
                    &MaterialAssetDescriptor::m_appearance, asset::metadata("appearance"),
                    config::EPresence::required)});
        }
    };

    struct BiomeAssetDescriptor : sim::asset::BiomeDescriptor {
        BiomeAppearance m_appearance{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple_cat(
                sim::asset::BiomeDescriptor::describe(),
                std::tuple{config::group(
                    &BiomeAssetDescriptor::m_appearance, asset::metadata("appearance"), config::EPresence::required)});
        }
    };

    struct FeatureAssetDescriptor : sim::asset::FeatureDescriptor {
        FeatureAppearance m_appearance{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple_cat(
                sim::asset::FeatureDescriptor::describe(),
                std::tuple{config::group(
                    &FeatureAssetDescriptor::m_appearance, asset::metadata("appearance"),
                    config::EPresence::required)});
        }
    };

    struct WorldAssetDescriptor : sim::asset::WorldDescriptor {
        WorldAppearance m_appearance{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple_cat(
                sim::asset::WorldDescriptor::describe(),
                std::tuple{config::group(
                    &WorldAssetDescriptor::m_appearance, asset::metadata("appearance"), config::EPresence::required)});
        }
    };

    using BackgroundAssetDescriptor = sim::asset::Descriptor<BackgroundAppearance>;

    struct BackgroundIdTag;
    using BackgroundId = Numeric<u64, BackgroundIdTag>;

    template<typename Descriptor, typename Id>
    struct ContentDescriptor {
        asset::Imported<Descriptor> m_descriptor{};
        Id m_id{};
    };

    using MaterialAsset = ContentDescriptor<MaterialAssetDescriptor, sim::asset::MaterialId>;
    using FieldAsset = ContentDescriptor<sim::asset::FieldDescriptor, sim::asset::FieldId>;
    using BiomeAsset = ContentDescriptor<BiomeAssetDescriptor, sim::asset::BiomeId>;
    using FeatureAsset = ContentDescriptor<FeatureAssetDescriptor, sim::asset::FeatureId>;
    using RuleAsset = ContentDescriptor<sim::asset::RuleDescriptor, sim::asset::RuleId>;
    using WorldAsset = ContentDescriptor<WorldAssetDescriptor, sim::asset::WorldId>;
    using BackgroundAsset = ContentDescriptor<BackgroundAssetDescriptor, BackgroundId>;

    /// A complete typed definition import pins its immutable pack metadata.
    /// Retained observers use safe_ptr only after the owning catalog is pinned;
    /// spans and entry pointers remain short-lived views of this owner.
    class ContentDescriptorCatalog : public safe_object {
        // owners:
        std::shared_ptr<const asset::PackSet> m_packs;
        sim::asset::ElementCatalog m_elements;
        image::TextureCatalog m_textures;

        // typed definitions:
        Array<MaterialAsset> m_materials;
        Array<FieldAsset> m_fields;
        Array<BiomeAsset> m_biomes;
        Array<FeatureAsset> m_features;
        Array<RuleAsset> m_rules;
        Array<WorldAsset> m_worlds;
        Array<BackgroundAsset> m_backgrounds;

        // construction and link validation:
        ContentDescriptorCatalog(
            std::shared_ptr<const asset::PackSet> packs, sim::asset::ElementCatalog elements,
            image::TextureCatalog textures);
        [[nodiscard]] std::error_code validateLinks(config::Diagnostics &diagnostics) const;

    public:
        [[nodiscard]] static Expected<ContentDescriptorCatalog> importDefinitions(
            std::shared_ptr<const asset::PackSet> packs, config::Diagnostics &diagnostics);

        [[nodiscard]] static Expected<ContentDescriptorCatalog> importDefinitions(
            std::shared_ptr<const asset::PackSet> packs, asset::DescriptorReader reader,
            config::Diagnostics &diagnostics);

        [[nodiscard]] const asset::PackSet &packs() const noexcept;
        [[nodiscard]] std::shared_ptr<const asset::PackSet> packOwner() const noexcept;
        [[nodiscard]] const sim::asset::ElementCatalog &elements() const noexcept;
        [[nodiscard]] const image::TextureCatalog &textures() const noexcept;
        [[nodiscard]] std::span<const MaterialAsset> materials() const noexcept;
        [[nodiscard]] std::span<const FieldAsset> fields() const noexcept;
        [[nodiscard]] std::span<const BiomeAsset> biomes() const noexcept;
        [[nodiscard]] std::span<const FeatureAsset> features() const noexcept;
        [[nodiscard]] std::span<const RuleAsset> rules() const noexcept;
        [[nodiscard]] std::span<const WorldAsset> worlds() const noexcept;
        [[nodiscard]] std::span<const BackgroundAsset> backgrounds() const noexcept;
    };

    // ------------------------------------------------------------------
    // assembled content definition
    // ------------------------------------------------------------------

    struct PackDefinition : sim::definition::Definition {
        Appearance m_appearance{};
        static constexpr auto describe() {
            return std::tuple_cat(
                sim::definition::Definition::describe(),
                std::tuple{config::group(
                    &PackDefinition::m_appearance, appearance_schema::metadata("appearance"),
                    config::EPresence::required)});
        }
    };

    // ------------------------------------------------------------------
    // selected world definition and numeric lowering
    // ------------------------------------------------------------------

    struct AssembledWorldDefinition {
        std::shared_ptr<const asset::PackSet> m_packs{};
        PackDefinition m_definition{};
        sim::definition::CompiledRecipe m_compiled{};
        Array<image::TextureDefinition> m_textures{};
        sim::asset::WorldId m_world{};
        u64 m_numeric_fingerprint{};
        /// Descriptor semantics only; source-byte identities enter explicit image preparation.
        u64 m_presentation_descriptor_fingerprint{};
    };

    [[nodiscard]] Expected<AssembledWorldDefinition> assembleWorld(
        const ContentDescriptorCatalog &catalog, sim::asset::WorldId world,
        sim::definition::GenerationCompileParameters parameters, config::Diagnostics &diagnostics);

    [[nodiscard]] Expected<config::Document> contentVariantSubtree(
        EContentDocument category, std::string_view definition_key, std::string_view relative_variant_path,
        std::string_view tag);

    // ------------------------------------------------------------------
    // content catalog
    // ------------------------------------------------------------------

    // Immutable owned material catalog. Decoding produces a candidate; callers
    // retain their previous catalog on failure. No parser views survive decoding.
    class ContentCatalog : public safe_object {
        // state:
        std::shared_ptr<const ContentDescriptorCatalog> m_descriptors{};
        PackDefinition m_definition{};
        std::optional<sim::definition::CompiledRecipe> m_selected_recipe{};
        Array<image::TextureDefinition> m_texture_definitions{};
        Array<MaterialDefinition> m_materials{};
        std::string m_pack_id{};
        Array<FieldDefinition> m_fields{};
        Array<BiomeDefinition> m_biomes{};
        Array<FeatureDefinition> m_features{};
        Array<WorldProfileDefinition> m_worlds{};
        Array<BackgroundStackDefinition> m_backgrounds{};
        std::optional<u64> m_content_fingerprint{};
        sim::asset::WorldId m_world{};
        u64 m_presentation_descriptor_fingerprint{};

        ContentCatalog() = default;

        [[nodiscard]] static Expected<ContentCatalog> project(
            PackDefinition definition, const config::Document &document,
            const sim::definition::CompiledDefinition *standalone, const sim::definition::CompiledRecipe *selected,
            config::Diagnostics &diagnostics);

        friend Expected<ContentCatalog> projectContentCatalog(
            PackDefinition, const config::Document &, config::Diagnostics &);
        friend Expected<ContentCatalog> selectWorldCatalog(
            std::shared_ptr<const ContentDescriptorCatalog>, sim::asset::WorldId,
            sim::definition::GenerationCompileParameters, config::Diagnostics &);

    public:
        explicit ContentCatalog(ZeroValue) noexcept : ContentCatalog() {
        }

        ContentCatalog(ContentCatalog &&) = default;
        ContentCatalog &operator=(ContentCatalog &&) = default;

        ContentCatalog(const ContentCatalog &) = default;
        ContentCatalog &operator=(const ContentCatalog &) = default;

        [[nodiscard]] const PackDefinition &definition() const noexcept;
        [[nodiscard]] const sim::definition::CompiledRecipe *selectedRecipe() const noexcept;
        [[nodiscard]] sim::asset::WorldId worldId() const noexcept;
        [[nodiscard]] std::span<const image::TextureDefinition> textureDefinitions() const noexcept;
        [[nodiscard]] const std::shared_ptr<const ContentDescriptorCatalog> &descriptorOwner() const noexcept;
        [[nodiscard]] u64 presentationDescriptorFingerprint() const noexcept;

        [[nodiscard]] std::span<const MaterialDefinition> materials() const noexcept;
        [[nodiscard]] const MaterialDefinition *findMaterial(u16 numeric_id) const noexcept;
        [[nodiscard]] const MaterialDefinition *findMaterial(std::string_view key) const noexcept;
        /// Resolves a nonzero runtime ID through this selected World's palette.
        [[nodiscard]] Expected<asset::QualifiedName> materialAssetName(
            u16 numeric_id, config::Diagnostics &diagnostics) const;
        // Physical semantics and numeric material bindings; presentation is separate.
        [[nodiscard]] u64 materialFingerprint() const;
        [[nodiscard]] std::string_view packId() const noexcept;
        [[nodiscard]] std::span<const FieldDefinition> fields() const noexcept;
        [[nodiscard]] std::span<const BiomeDefinition> biomes() const noexcept;
        [[nodiscard]] std::span<const FeatureDefinition> features() const noexcept;
        [[nodiscard]] std::span<const WorldProfileDefinition> worlds() const noexcept;
        [[nodiscard]] std::span<const BackgroundStackDefinition> backgrounds() const noexcept;
        [[nodiscard]] const WorldProfileDefinition *findWorld(std::string_view key) const noexcept;
        // Present on a selected world compiled with its actual parameters.
        [[nodiscard]] std::optional<u64> contentFingerprint() const noexcept;
    };

    using SharedContentCatalog = safe_ptr<const ContentCatalog>;

    // ------------------------------------------------------------------
    // explicitly prepared presentation sidecars
    // ------------------------------------------------------------------

    class ContentPresentationRequest;

    /// Identity only: a fresh token belongs to one explicit Open/Reload candidate.
    /// Binary replacements are adopted by a fresh epoch; null disables reuse.
    class ContentSourceEpoch;
    using ContentSourceEpochToken = std::shared_ptr<const ContentSourceEpoch>;

    [[nodiscard]] ContentSourceEpochToken makeContentSourceEpoch();

    class ContentPresentation : public safe_object {
        std::shared_ptr<const ContentCatalog> m_catalog{};
        ContentSourceEpochToken m_source_epoch{};
        Array<ContentTexture> m_textures{};
        asset::RequestGeneration m_generation{};
        u64 m_fingerprint{};
        image::TexturePreparationWork m_work{};
        u32 m_processing_revision{};

        ContentPresentation(
            std::shared_ptr<const ContentCatalog> catalog, Array<ContentTexture> textures,
            asset::RequestGeneration generation, u64 fingerprint, ContentSourceEpochToken source_epoch,
            u32 processing_revision, image::TexturePreparationWork work);
        friend class ContentPresentationRequest;

    public:
        [[nodiscard]] const std::shared_ptr<const ContentCatalog> &catalog() const noexcept;
        [[nodiscard]] std::span<const ContentTexture> textures() const noexcept;
        [[nodiscard]] asset::RequestGeneration generation() const noexcept;
        [[nodiscard]] u64 fingerprint() const noexcept;
        [[nodiscard]] const ContentSourceEpochToken &sourceEpoch() const noexcept;
        [[nodiscard]] u32 processingRevision() const noexcept;
        [[nodiscard]] const image::TexturePreparationWork &work() const noexcept;
    };

    /// Composition only: the image processor and shared asset request own every
    /// asynchronous lifecycle transition. Take/finish publishes a matched sidecar.
    class ContentPresentationRequest {
        std::shared_ptr<const ContentCatalog> m_catalog{};
        ContentSourceEpochToken m_source_epoch{};
        image::TexturePreparation m_request;

        ContentPresentationRequest(
            std::shared_ptr<const ContentCatalog> catalog, image::TexturePreparation request,
            ContentSourceEpochToken source_epoch);
        [[nodiscard]] Expected<std::shared_ptr<const ContentPresentation>> complete(
            Expected<image::TexturePreparation::Result> result, config::Diagnostics &diagnostics);

    public:
        ContentPresentationRequest(ContentPresentationRequest &&) noexcept = default;
        ContentPresentationRequest &operator=(ContentPresentationRequest &&) = delete;

        ContentPresentationRequest(const ContentPresentationRequest &) = delete;
        ContentPresentationRequest &operator=(const ContentPresentationRequest &) = delete;

        [[nodiscard]] static Expected<ContentPresentationRequest> start(
            std::shared_ptr<const ContentCatalog> catalog, asset::RequestGeneration generation, SharedContext context,
            config::Diagnostics &diagnostics, ContentSourceEpochToken source_epoch = {},
            const std::shared_ptr<const ContentPresentation> &previous = {});
        [[nodiscard]] bool poll() noexcept;
        void cancel() noexcept;
        [[nodiscard]] asset::PreparationProgress progress() const noexcept;
        [[nodiscard]] Expected<std::shared_ptr<const ContentPresentation>> take(config::Diagnostics &diagnostics);
        [[nodiscard]] Expected<std::shared_ptr<const ContentPresentation>> finish(config::Diagnostics &diagnostics);
    };

    [[nodiscard]] Expected<sim::asset::WorldId> resolveContentWorld(
        const ContentDescriptorCatalog &catalog, std::string_view name, config::Diagnostics &diagnostics);
    [[nodiscard]] Expected<ContentCatalog> selectWorldCatalog(
        std::shared_ptr<const ContentDescriptorCatalog> catalog, sim::asset::WorldId world,
        sim::definition::GenerationCompileParameters parameters, config::Diagnostics &diagnostics);

    [[nodiscard]] Expected<ContentCatalog> projectContentCatalog(
        PackDefinition definition, const config::Document &document, config::Diagnostics &diagnostics);
    [[nodiscard]] Expected<ContentCatalog> projectContentCatalog(
        PackDefinition definition, config::Diagnostics &diagnostics);

    [[nodiscard]] Expected<sim::definition::Definition> decodeWorldGenDefinition(
        std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);

    /// Definition-only import. Binary texture preparation is an explicit sidecar.
    [[nodiscard]] Expected<ContentDescriptorCatalog> loadContentCatalog(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *context = nullptr);
    [[nodiscard]] Expected<ContentDescriptorCatalog> loadContentCatalog(
        std::span<const std::filesystem::path> roots, config::Diagnostics &diagnostics,
        const IContext *context = nullptr);

    [[nodiscard]] Expected<ContentDescriptorCatalog> loadContentCatalog(
        const ContentSourceSet &source, config::Diagnostics &diagnostics, const IContext *context = nullptr);
    [[nodiscard]] Expected<ContentDescriptorCatalog> loadContentCatalog(
        const ContentSourceSnapshot &source, config::Diagnostics &diagnostics, const IContext *context = nullptr);
}
