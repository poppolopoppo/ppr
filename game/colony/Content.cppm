module;

export module game.colony.content;

import engine.core;
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

    inline constexpr std::size_t kMaxMaterialDocumentBytes = 1024u * 1024u;
    inline constexpr std::size_t kMaxContentMaterials = 256u;
    inline constexpr std::size_t kMaxContentFields = 256u;
    inline constexpr std::size_t kMaxContentBiomes = 64u;
    inline constexpr std::size_t kMaxBiomeCompositionBands = 16u;
    inline constexpr std::size_t kMaxCompositionMaterials = 64u;
    inline constexpr std::size_t kMaxBiomeDeposits = 64u;
    inline constexpr std::size_t kMaxContentFeatures = 128u;
    inline constexpr std::size_t kMaxFeatureOperations = 128u;
    inline constexpr std::size_t kMaxFeatureMarkers = 64u;
    inline constexpr std::size_t kMaxContentWorlds = 16u;
    inline constexpr std::size_t kMaxWorldRules = 128u;
    inline constexpr std::size_t kMaxWorldSites = 4096u;
    inline constexpr std::size_t kMaxStartResources = 8u;
    inline constexpr std::size_t kMaxPackManifestBytes = 64u * 1024u;

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
        appearance,
        count
    };
    inline constexpr std::size_t kContentDocumentCount = static_cast<std::size_t>(EContentDocument::count);

    struct ContentDocumentBinding {
        EContentDocument m_document{};
        std::string_view m_collection{}, m_label{}, m_schema_root{};
        bool m_attached_rules{};
        std::size_t m_definition_depth{1u};
    };
    [[nodiscard]] std::span<const ContentDocumentBinding> contentDocumentBindings() noexcept;
    [[nodiscard]] std::string contentDocumentPath(
        EContentDocument category, std::string_view definition_key, std::string_view relative_property = {});
    [[nodiscard]] std::string contentCanonicalPath(EContentDocument category, std::string_view document_path);
    [[nodiscard]] std::optional<std::pair<EContentDocument, std::string>> contentSourcePath(
        std::string_view canonical_path);
    [[nodiscard]] Array<std::string> contentDefinitionKeys(EContentDocument category, const config::Document &document);

    /// Numeric limits and enum choices come from the decoding schemas. Slider
    /// bounds are viewport hints only; editors must include the current value.
    /// Choice lists are owned; their string views have immutable static lifetime.
    struct ContentPropertyDescriptor : config::PropertyDescriptor {
        std::optional<EContentDocument> m_reference_category{};
    };
    [[nodiscard]] std::optional<ContentPropertyDescriptor> contentPropertyDescriptor(
        EContentDocument category, std::string_view relative_property, const config::Document &document,
        std::string_view definition_key);
    /// Contextual reference choices from current draft documents, including
    /// profile-enabled biomes, feature rules and their selected template markers.
    [[nodiscard]] Array<std::string> contentPropertyReferenceChoices(
        EContentDocument category, std::string_view relative_property,
        const std::array<config::Document, kContentDocumentCount> &documents, std::string_view definition_key);

    // ------------------------------------------------------------------
    // manifests
    // ------------------------------------------------------------------

    struct ContentPackManifest {
        std::string m_pack_id{};
        // Indexed by EContentDocument; the manifest is the complete file set.
        std::array<std::string, kContentDocumentCount> m_files{};
        std::array<std::string, kContentDocumentCount> m_file_locations{};
    };
    struct ContentSourceDocument {
        std::filesystem::path m_path{};
        std::string m_bytes{};
    };
    struct ContentPackSource {
        ContentPackManifest m_manifest{};
        ContentSourceDocument m_manifest_source{};
        std::array<ContentSourceDocument, kContentDocumentCount> m_documents{};
    };

    // ------------------------------------------------------------------
    // fields
    // ------------------------------------------------------------------

    struct FieldDefinition {
        std::string m_key{};
        sim::NoiseFieldDesc m_noise{};
    };

    // Owned, canonically sorted named fields; numeric validation reuses sim's
    // allocation-free compiler preflight. No noise object or parser view survives.
    [[nodiscard]] Expected<ContentPackManifest> decodePackManifest(
        std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);
    /// Blocking I/O boundary: call on the world coordinator, never the UI lane.
    /// Resolves an explicit root, checks containment and size before mapping,
    /// and returns owned bytes. No application/service lookup or parser views.
    [[nodiscard]] Expected<ContentPackSource> loadContentPackSource(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *context = nullptr);

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

    inline constexpr u32 kContentSchemaVersion = 3u;
    inline constexpr u64 kMaxTextureInputBytes = 16u * 1024u * 1024u;
    inline constexpr u64 kMaxPackTextureInputBytes = 64u * 1024u * 1024u;
    inline constexpr u64 kMaxPackTextureDecodedBytes = 128u * 1024u * 1024u;
    inline constexpr u32 kMaxContentTextureDimension = 2048u;
    inline constexpr u32 kMaxContentTextures = 256u;
    struct ContentTexture {
        std::string m_path{};
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
                    &MaterialAppearance::m_albedo_texture, metadata("albedo_texture"), StringDomain{1024u, false},
                    EPresence::optional),
                field(
                    &MaterialAppearance::m_normal_texture, metadata("normal_texture"), StringDomain{1024u, false},
                    EPresence::optional),
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
                    config::StringDomain{128u, false}, config::EPresence::required)};
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
                    &BackgroundAppearanceLayer::m_texture, metadata("texture"), StringDomain{1024u, false},
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

    [[nodiscard]] Expected<config::Document> projectContentDocument(
        EContentDocument category, const PackDefinition &definition);
    [[nodiscard]] Expected<config::Document> contentVariantSubtree(
        EContentDocument category, std::string_view definition_key, std::string_view relative_variant_path,
        std::string_view tag);
    [[nodiscard]] Expected<config::Document> mergeContentDocuments(
        const std::array<config::Document, kContentDocumentCount> &documents, config::Diagnostics &diagnostics);
    [[nodiscard]] Expected<PackDefinition> decodePackDefinition(
        const ContentPackSource &source, config::Diagnostics &diagnostics);
    [[nodiscard]] Expected<PackDefinition> resolvePackDefinition(
        const config::Document &document, config::Diagnostics &diagnostics);

    // ------------------------------------------------------------------
    // content catalog
    // ------------------------------------------------------------------

    // Immutable owned material catalog. Decoding produces a candidate; callers
    // retain their previous catalog on failure. No parser views survive decoding.
    class ContentCatalog : public safe_object {
        // state:
        PackDefinition m_definition{};
        Array<MaterialDefinition> m_materials{};
        std::string m_pack_id{};
        Array<FieldDefinition> m_fields{};
        Array<BiomeDefinition> m_biomes{};
        Array<FeatureDefinition> m_features{};
        Array<WorldProfileDefinition> m_worlds{};
        Array<ContentTexture> m_textures{};
        Array<BackgroundStackDefinition> m_backgrounds{};
        std::optional<u64> m_content_fingerprint{};

        // friends:
        friend Expected<ContentCatalog> loadContentCatalog(
            const std::filesystem::path &, config::Diagnostics &, const IContext *);
        friend Expected<ContentCatalog> loadContentCatalog(
            const std::filesystem::path &, const ContentPackSource &, config::Diagnostics &, const IContext *);

        [[nodiscard]] u64 computeContentFingerprint() const;

        ContentCatalog() = default;

        friend Expected<ContentCatalog> projectContentCatalog(
            PackDefinition, const config::Document &, config::Diagnostics &);

    public:
        explicit ContentCatalog(ZeroValue) noexcept : ContentCatalog() {
        }

        ContentCatalog(ContentCatalog &&) = default;
        ContentCatalog &operator=(ContentCatalog &&) = default;

        ContentCatalog(const ContentCatalog &) = default;
        ContentCatalog &operator=(const ContentCatalog &) = default;

        [[nodiscard]] const PackDefinition &definition() const noexcept;

        [[nodiscard]] std::span<const MaterialDefinition> materials() const noexcept;
        [[nodiscard]] const MaterialDefinition *findMaterial(u16 numeric_id) const noexcept;
        [[nodiscard]] const MaterialDefinition *findMaterial(std::string_view key) const noexcept;
        // Rapidhash block chaining over canonical material values, with explicit
        // lengths and little-endian numeric bytes. Not the complete pack identity.
        [[nodiscard]] u64 materialFingerprint() const;
        [[nodiscard]] std::string_view packId() const noexcept;
        [[nodiscard]] std::span<const FieldDefinition> fields() const noexcept;
        [[nodiscard]] std::span<const BiomeDefinition> biomes() const noexcept;
        [[nodiscard]] std::span<const FeatureDefinition> features() const noexcept;
        [[nodiscard]] std::span<const WorldProfileDefinition> worlds() const noexcept;
        [[nodiscard]] std::span<const ContentTexture> textures() const noexcept;
        [[nodiscard]] std::span<const BackgroundStackDefinition> backgrounds() const noexcept;
        [[nodiscard]] const WorldProfileDefinition *findWorld(std::string_view key) const noexcept;
        // Present only after complete pack and asset validation.
        [[nodiscard]] std::optional<u64> contentFingerprint() const noexcept;
    };

    using SharedContentCatalog = safe_ptr<const ContentCatalog>;

    [[nodiscard]] Expected<ContentCatalog> projectContentCatalog(
        PackDefinition definition, const config::Document &document, config::Diagnostics &diagnostics);
    [[nodiscard]] Expected<ContentCatalog> projectContentCatalog(
        PackDefinition definition, config::Diagnostics &diagnostics);

    [[nodiscard]] Expected<sim::definition::Definition> decodeWorldGenDefinition(
        std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);

    // Worker/headless boundary: all documents and referenced textures are validated
    // before a complete owned candidate is returned. No GPU or services are accessed.
    [[nodiscard]] Expected<ContentCatalog> loadContentCatalog(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *context = nullptr);
    [[nodiscard]] Expected<ContentCatalog> loadContentCatalog(
        const std::filesystem::path &root, const ContentPackSource &source, config::Diagnostics &diagnostics,
        const IContext *context = nullptr);
}
