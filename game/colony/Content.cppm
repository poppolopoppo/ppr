module;

export module game.colony.content;

import engine.core;
import engine.sim;
import engine.image;
import std;

export namespace pP::colony {
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

    enum class EContentDocument : u8 {
        materials,
        biomes,
        fields,
        features,
        worlds,
        backgrounds,
        count
    };
    inline constexpr std::size_t kContentDocumentCount = static_cast<std::size_t>(EContentDocument::count);

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
        std::array<ContentSourceDocument, kContentDocumentCount> m_documents{};
    };

    struct FieldDefinition {
        std::string m_key{};
        sim::NoiseFieldDesc m_noise{};
    };

    // Owned, canonically sorted named fields; numeric validation reuses sim's
    // allocation-free compiler preflight. No noise object or parser view survives.
    [[nodiscard]] Expected<Array<FieldDefinition>> decodeFieldDefinitions(
        std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);

    [[nodiscard]] Expected<ContentPackManifest> decodePackManifest(
        std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);
    /// Blocking I/O boundary: call on the world coordinator, never the UI lane.
    /// Resolves an explicit root, checks containment and size before mapping,
    /// and returns owned bytes. No application/service lookup or parser views.
    [[nodiscard]] Expected<ContentPackSource> loadContentPackSource(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *context = nullptr);

    class ContentCatalog;

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
    };

    // Pure, owned biome recipes with terrain references resolved to validated
    // numeric IDs. Fields retain their stable stream keys. Explicit ordered
    // composition bands cover normalized depth [0,1] without gaps/overlap.
    [[nodiscard]] Expected<Array<BiomeDefinition>> decodeBiomeDefinitions(
        std::string_view bytes, std::string_view origin, const ContentCatalog &materials,
        std::span<const FieldDefinition> fields, config::Diagnostics &diagnostics);

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
    [[nodiscard]] Expected<Array<FeatureDefinition>> decodeFeatureDefinitions(
        std::string_view bytes, std::string_view origin, const ContentCatalog &materials,
        config::Diagnostics &diagnostics);

    enum class EWorldRuleKind : u8 {
        threshold_cave,
        carve_worm,
        vein_worm,
        connect_rooms,
        feature,
        matter_fill
    };
    enum class EWorldRulePhase : u8 {
        deposit,
        carve,
        stamp,
        fill
    };
    struct WorldBiomeChoice {
        std::string m_key{};
        std::string m_biome{};
        double m_weight{};
        u32 m_required_count{};
        double m_depth_min_normalized{}, m_depth_max_normalized{};
        double m_distance_min_m{}, m_distance_max_m{};
    };
    struct WorldCaveRule {
        std::string m_biome{}, m_field{};
        double m_depth_min_normalized{}, m_depth_max_normalized{};
        double m_threshold_min{}, m_threshold_max{};
    };
    struct WorldWormRule {
        std::string m_biome{};
        u32 m_count{};
        double m_depth_min_normalized{}, m_depth_max_normalized{};
        double m_length_min_m{}, m_length_max_m{};
        double m_radius_min_m{}, m_radius_max_m{};
        double m_turn_probability{};
        u16 m_material_id{}, m_host_material_id{};
    };
    struct WorldConnectionRule {
        double m_radius_m{};
        u32 m_extra_links{};
    };
    struct WorldFeatureRule {
        std::string m_feature{}, m_biome{};
        double m_depth_min_normalized{}, m_depth_max_normalized{};
        double m_spacing_m{};
        u32 m_min_count{}, m_max_count{}, m_attempts{}, m_priority{};
        bool m_reflect{};
    };
    struct WorldMatterRule {
        std::string m_feature_rule{}, m_marker{};
        u16 m_material_id{};
        double m_temperature_k{}, m_bulk_density_kg_m3{};
        std::optional<double> m_fill_height_m{};
    };
    struct WorldRuleDefinition {
        std::string m_key{};
        u32 m_order{};
        EWorldRuleKind m_kind{};
        EWorldRulePhase m_phase{};
        std::variant<WorldCaveRule, WorldWormRule, WorldConnectionRule, WorldFeatureRule, WorldMatterRule>
            m_parameters{};
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
        Array<WorldRuleDefinition> m_rules{};
        std::string m_background_stack{};
    };
    struct BackgroundLayerDefinition {
        std::string m_key{}, m_texture{}, m_source_location{};
        u32 m_order{}, m_color_rgba{};
        double m_repeat_period_m{}, m_parallax_factor{};
    };
    struct BackgroundStackDefinition {
        std::string m_key{};
        Array<BackgroundLayerDefinition> m_layers{};
    };
    [[nodiscard]] Expected<Array<BackgroundStackDefinition>> decodeBackgroundDefinitions(
        std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);
    inline constexpr u32 kContentSchemaVersion = 2u;
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
    // Immutable owned material catalog. Decoding produces a candidate; callers
    // retain their previous catalog on failure. No parser views survive decoding.
    class ContentCatalog {
        Array<MaterialDefinition> m_materials{};
        std::string m_pack_id{};
        Array<FieldDefinition> m_fields{};
        Array<BiomeDefinition> m_biomes{};
        Array<FeatureDefinition> m_features{};
        Array<WorldProfileDefinition> m_worlds{};
        Array<ContentTexture> m_textures{};
        Array<BackgroundStackDefinition> m_backgrounds{};
        std::optional<u64> m_content_fingerprint{};
        friend Expected<ContentCatalog> loadContentCatalog(
            const std::filesystem::path &, config::Diagnostics &, const IContext *);
        [[nodiscard]] u64 computeContentFingerprint() const noexcept;
        ContentCatalog() = default;
        friend Expected<ContentCatalog> decodeMaterialCatalog(
            std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);

    public:
        ContentCatalog(ContentCatalog &&) = default;
        ContentCatalog &operator=(ContentCatalog &&) = default;
        ContentCatalog(const ContentCatalog &) = default;
        ContentCatalog &operator=(const ContentCatalog &) = default;

        [[nodiscard]] std::span<const MaterialDefinition> materials() const noexcept;
        [[nodiscard]] const MaterialDefinition *findMaterial(u16 numeric_id) const noexcept;
        [[nodiscard]] const MaterialDefinition *findMaterial(std::string_view key) const noexcept;
        // FNV-1a over the versioned canonical material values, with explicit
        // lengths and little-endian numeric bytes. Not the complete pack identity.
        [[nodiscard]] u64 materialFingerprint() const noexcept;
        [[nodiscard]] std::string_view packId() const noexcept;
        [[nodiscard]] std::span<const FieldDefinition> fields() const noexcept;
        [[nodiscard]] std::span<const BiomeDefinition> biomes() const noexcept;
        [[nodiscard]] std::span<const FeatureDefinition> features() const noexcept;
        [[nodiscard]] std::span<const WorldProfileDefinition> worlds() const noexcept;
        [[nodiscard]] std::span<const ContentTexture> textures() const noexcept;
        [[nodiscard]] std::span<const BackgroundStackDefinition> backgrounds() const noexcept;
        [[nodiscard]] const WorldProfileDefinition *findWorld(std::string_view key) const noexcept;
        // Absent for the material-only decoder; present only after complete pack validation.
        [[nodiscard]] std::optional<u64> contentFingerprint() const noexcept;
    };

    [[nodiscard]] Expected<Array<WorldProfileDefinition>> decodeWorldProfiles(
        std::string_view bytes, std::string_view origin, const ContentCatalog &materials,
        std::span<const FieldDefinition> fields, std::span<const BiomeDefinition> biomes,
        std::span<const FeatureDefinition> features, config::Diagnostics &diagnostics);

    // Pure scalar-codec boundary; performs no I/O or texture decoding. Terrain
    // texture paths are portable pack-relative paths without dot segments.
    // bad_alloc propagates to the application's fatal exception boundary.
    [[nodiscard]] Expected<ContentCatalog> decodeMaterialCatalog(
        std::string_view bytes, std::string_view origin, config::Diagnostics &diagnostics);
    // Worker/headless boundary: all documents and referenced textures are validated
    // before a complete owned candidate is returned. No GPU or services are accessed.
    [[nodiscard]] Expected<ContentCatalog> loadContentCatalog(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *context = nullptr);
}
