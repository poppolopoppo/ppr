module;

#include "pP/Macros.h"

module game.colony.content;

import engine.core;
import engine.sim;
import game.colony.metric;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyContent, info, none)

    namespace {
        using namespace config;
        using pP::make_error_code;

        constexpr Metadata metadata(
            const std::string_view name, const std::string_view reference = {}, const std::string_view units = {},
            const std::optional<double> slider_min = {}, const std::optional<double> slider_max = {},
            const std::string_view description = "World content field", const bool read_only = false) {
            Metadata result{name, description, "World content", EMutability::startup, ESource::file, false};
            result.m_reference_category = reference;
            result.m_units = units;
            result.m_slider_min = slider_min;
            result.m_slider_max = slider_max;
            result.m_read_only = read_only;
            return result;
        }

        struct PackManifestRecord {
            u32 m_schema_version{};
            std::string m_pack_id{};
            std::string m_materials{};
            std::string m_biomes{};
            std::string m_fields{};
            std::string m_features{};
            std::string m_worlds{};
            std::string m_backgrounds{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &PackManifestRecord::m_schema_version, metadata("schema_version"),
                        Range<u32>{kContentSchemaVersion, kContentSchemaVersion}),
                    field(&PackManifestRecord::m_pack_id, metadata("pack_id"), StringDomain{128u, false}),
                    field(&PackManifestRecord::m_materials, metadata("files.materials"), StringDomain{1024u, false}),
                    field(&PackManifestRecord::m_biomes, metadata("files.biomes"), StringDomain{1024u, false}),
                    field(&PackManifestRecord::m_fields, metadata("files.fields"), StringDomain{1024u, false}),
                    field(&PackManifestRecord::m_features, metadata("files.features"), StringDomain{1024u, false}),
                    field(&PackManifestRecord::m_worlds, metadata("files.worlds"), StringDomain{1024u, false}),
                    field(
                        &PackManifestRecord::m_backgrounds, metadata("files.backgrounds"), StringDomain{1024u, false})};
            }
        };

        struct BackgroundLayerRecord {
            u32 m_order{};
            std::string m_texture{}, m_color_rgba{};
            double m_repeat_period_m{}, m_parallax_factor{};
            static constexpr auto describe() {
                return std::tuple{
                    field(&BackgroundLayerRecord::m_order, metadata("order"), Range<u32>{0u, 3u}),
                    field(&BackgroundLayerRecord::m_texture, metadata("texture"), StringDomain{1024u, false}),
                    field(&BackgroundLayerRecord::m_color_rgba, metadata("color_rgba"), StringDomain{8u, false}),
                    field(
                        &BackgroundLayerRecord::m_repeat_period_m, metadata("repeat_period_m", {}, "m"),
                        FloatDomain{0.125, 65536.0, false, false}),
                    field(
                        &BackgroundLayerRecord::m_parallax_factor, metadata("parallax_factor"),
                        FloatDomain{0.0, 1.0, false, false})};
            }
        };

        struct FieldRecord {
            sim::ENoiseAlgorithm m_algorithm{};
            sim::ENoiseFractal m_fractal{sim::ENoiseFractal::none};
            double m_frequency_per_metre{};
            u32 m_octaves{4u};
            double m_lacunarity{2.0};
            double m_gain{0.5};
            double m_warp_amplitude_m{};
            double m_warp_frequency_per_metre{0.006};
            double m_axis_scale_x{1.0}, m_axis_scale_y{1.0}, m_rotation_degrees{};

            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &FieldRecord::m_algorithm, metadata("algorithm"),
                        EnumDomain<sim::ENoiseAlgorithm, 4>{
                            {EnumEntry<sim::ENoiseAlgorithm>{sim::ENoiseAlgorithm::open_simplex2, "open_simplex2"},
                                EnumEntry<sim::ENoiseAlgorithm>{sim::ENoiseAlgorithm::perlin, "perlin"},
                                EnumEntry<sim::ENoiseAlgorithm>{
                                    sim::ENoiseAlgorithm::cellular_distance, "cellular_distance"},
                                EnumEntry<sim::ENoiseAlgorithm>{
                                    sim::ENoiseAlgorithm::cellular_value, "cellular_value"}}}),
                    field(
                        &FieldRecord::m_fractal, metadata("fractal"),
                        EnumDomain<sim::ENoiseFractal, 4>{
                            {EnumEntry<sim::ENoiseFractal>{sim::ENoiseFractal::none, "none"},
                                EnumEntry<sim::ENoiseFractal>{sim::ENoiseFractal::fbm, "fbm"},
                                EnumEntry<sim::ENoiseFractal>{sim::ENoiseFractal::ridged, "ridged"},
                                EnumEntry<sim::ENoiseFractal>{sim::ENoiseFractal::billow, "billow"}}}),
                    field(
                        &FieldRecord::m_frequency_per_metre,
                        metadata("frequency_per_metre", {}, "1/m", 0.0, 0.1, "Noise frequency in cycles per metre."),
                        FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}),
                    field(&FieldRecord::m_octaves, metadata("octaves"), Range<u32>{1u, 16u}),
                    field(&FieldRecord::m_lacunarity, metadata("lacunarity"), FloatDomain{1.0, 4.0, false, false}),
                    field(&FieldRecord::m_gain, metadata("gain"), FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &FieldRecord::m_axis_scale_x, metadata("axis_scale_x"), FloatDomain{0.125, 8.0, false, false}),
                    field(
                        &FieldRecord::m_axis_scale_y, metadata("axis_scale_y"), FloatDomain{0.125, 8.0, false, false}),
                    field(
                        &FieldRecord::m_rotation_degrees, metadata("rotation_degrees", {}, "degrees"),
                        FloatDomain{-180.0, 180.0, false, false}),
                    field(
                        &FieldRecord::m_warp_amplitude_m, metadata("warp_amplitude_m", {}, "m", 0.0, 100.0),
                        FloatDomain{0.0, sim::kNoiseCoordinateLimitMetres, false, false}),
                    field(
                        &FieldRecord::m_warp_frequency_per_metre,
                        metadata(
                            "warp_frequency_per_metre", {}, "1/m", 0.0, 0.1, "Frequency of the displacement noise."),
                        FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true})};
            }
        };

        struct BiomeRecord {
            std::string m_display_name{};
            std::string m_preview_color_rgba{};
            double m_temperature_base_k{};
            double m_temperature_variation_k{};
            std::string m_temperature_field{};
            std::optional<std::string> m_lining_material{};
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
            std::optional<std::string> m_soil_material{};
            std::optional<std::string> m_weathered_material{};
            double m_soil_depth_m{};
            double m_weathered_depth_m{};
            std::optional<std::string> m_formation_material{};
            std::optional<std::string> m_rind_material{};
            double m_formation_probability{};
            double m_formation_length_m{3.0};
            u32 m_erosion_passes{};
            static constexpr auto describe() {
                constexpr double limit = static_cast<double>((std::numeric_limits<float>::max)()) / 2.0;
                return std::tuple{
                    field(&BiomeRecord::m_pore_field, metadata("pore_field", "fields"), StringDomain{128u, false}),
                    field(
                        &BiomeRecord::m_pore_probability, metadata("pore_probability"),
                        FloatDomain{0.0, 0.2, false, false}),
                    field(
                        &BiomeRecord::m_pore_radius_min_m, metadata("pore_radius_min_m", {}, "m"),
                        FloatDomain{0.0, 3.0, false, false}),
                    field(
                        &BiomeRecord::m_pore_radius_max_m, metadata("pore_radius_max_m", {}, "m"),
                        FloatDomain{0.0, 3.0, false, false}),
                    field(
                        &BiomeRecord::m_pore_affinity_min, metadata("pore_affinity_min"),
                        FloatDomain{-1.0, 1.0, false, false}),
                    field(
                        &BiomeRecord::m_pore_passes,
                        metadata("pore_passes", {}, {}, {}, {}, "Neighbor passes that shape clustered pores."),
                        Range<u32>{0u, 6u}),
                    field(
                        &BiomeRecord::m_finish_passes,
                        metadata("finish_passes", {}, {}, {}, {}, "Solid-only neighbor passes that finish terrain."),
                        Range<u32>{0u, 4u}),
                    field(
                        &BiomeRecord::m_erosion_passes,
                        metadata("erosion_passes", {}, {}, {}, {}, "Final neighbor passes including empty space."),
                        Range<u32>{0u, 24u}),
                    field(
                        &BiomeRecord::m_finish_adoption, metadata("finish_adoption"),
                        FloatDomain{0.0, 0.3, false, false}),
                    field(
                        &BiomeRecord::m_soil_material, metadata("soil_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &BiomeRecord::m_weathered_material, metadata("weathered_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &BiomeRecord::m_soil_depth_m, metadata("soil_depth_m", {}, "m"),
                        FloatDomain{0.0, 12.0, false, false}),
                    field(
                        &BiomeRecord::m_weathered_depth_m, metadata("weathered_depth_m", {}, "m"),
                        FloatDomain{0.0, 30.0, false, false}),
                    field(
                        &BiomeRecord::m_formation_material, metadata("formation_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &BiomeRecord::m_rind_material, metadata("rind_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &BiomeRecord::m_formation_probability,
                        metadata(
                            "formation_probability", {}, {}, {}, {},
                            "Chance of attempting a supported roof or floor formation."),
                        FloatDomain{0.0, 0.2, false, false}),
                    field(
                        &BiomeRecord::m_formation_length_m,
                        metadata("formation_length_m", {}, "m", {}, {}, "Maximum roof or floor formation length."),
                        FloatDomain{0.0, 8.0, false, false}),

                    field(&BiomeRecord::m_display_name, metadata("display_name"), StringDomain{256u, false}),
                    field(&BiomeRecord::m_preview_color_rgba, metadata("preview_color_rgba"), StringDomain{8u, false}),
                    field(
                        &BiomeRecord::m_temperature_base_k,
                        metadata("temperature_base_k", {}, "K", 200.0, 1000.0, "Base terrain temperature."),
                        FloatDomain{0.0, limit, false, true}),
                    field(
                        &BiomeRecord::m_temperature_variation_k,
                        metadata(
                            "temperature_variation_k", {}, "K", 0.0, 200.0,
                            "Temperature noise amplitude around the base."),
                        FloatDomain{0.0, limit, false, false}),
                    field(
                        &BiomeRecord::m_temperature_field, metadata("temperature_field", "fields"),
                        StringDomain{128u, false}),
                    field(
                        &BiomeRecord::m_lining_material, metadata("lining_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &BiomeRecord::m_lining_thickness_fine, metadata("lining_thickness_fine", {}, "fine cells"),
                        Range<u32>{0u, 3u}),
                    field(
                        &BiomeRecord::m_relaxation_passes,
                        metadata(
                            "relaxation_passes", {}, {}, {}, {}, "Neighbor passes that relax material competition."),
                        Range<u32>{0u, 24u}),
                    field(
                        &BiomeRecord::m_relaxation_adoption, metadata("relaxation_adoption"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &BiomeRecord::m_sprinkle_probability, metadata("sprinkle_probability"),
                        FloatDomain{0.0, 1.0, false, false})};
            }
        };

        struct CompositionRecord {
            u32 m_order{};
            double m_depth_min_normalized{};
            double m_depth_max_normalized{};
            std::string m_field{};
            double m_bed_period_m{}, m_bed_warp_m{}, m_bed_dip{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &CompositionRecord::m_bed_period_m,
                        metadata("bed_period_m", {}, "m", {}, {}, "Depth interval between repeating geological beds."),
                        FloatDomain{0.0, 256.0, false, false}),
                    field(
                        &CompositionRecord::m_bed_warp_m,
                        metadata("bed_warp_m", {}, "m", {}, {}, "Noise displacement of geological beds."),
                        FloatDomain{0.0, 64.0, false, false}),
                    field(
                        &CompositionRecord::m_bed_dip,
                        metadata("bed_dip", {}, {}, {}, {}, "Horizontal slope of the geological beds."),
                        FloatDomain{-0.5, 0.5, false, false}),
                    field(&CompositionRecord::m_order, metadata("order"), Range<u32>{0u, 65535u}),
                    field(
                        &CompositionRecord::m_depth_min_normalized, metadata("depth_min_normalized"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &CompositionRecord::m_depth_max_normalized, metadata("depth_max_normalized"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(&CompositionRecord::m_field, metadata("field", "fields"), StringDomain{128u, false})};
            }
        };

        struct WeightedMaterialRecord {
            std::string m_material{};
            double m_weight{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &WeightedMaterialRecord::m_material, metadata("material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &WeightedMaterialRecord::m_weight,
                        metadata("weight", {}, {}, 0.0, 10.0, "Relative likelihood among competing choices."),
                        FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true})};
            }
        };

        struct DepositRecord {
            enum class Kind : u8 {
                threshold
            };
            Kind m_kind{};
            u32 m_order{};
            std::string m_field{};
            std::string m_material{};
            std::string m_host_material{};
            double m_depth_min_normalized{};
            double m_depth_max_normalized{};
            double m_threshold_min{-1.0};
            double m_threshold_max{1.0};
            sim::EGenerationDepositShape m_shape{sim::EGenerationDepositShape::field};
            sim::EGenerationDepositStage m_stage{sim::EGenerationDepositStage::bulk};
            u32 m_blob_count{};
            double m_radius_min_m{}, m_radius_max_m{};
            double m_stretch_min{1.0}, m_stretch_max{1.0}, m_distortion{};
            double m_core_coverage{1.0}, m_fringe_coverage{1.0}, m_sprinkle_probability{};
            double m_orientation_min_degrees{}, m_orientation_max_degrees{360.0};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &DepositRecord::m_kind, metadata("kind"),
                        EnumDomain<Kind, 1>{{EnumEntry<Kind>{Kind::threshold, "threshold"}}}),
                    field(&DepositRecord::m_order, metadata("order"), Range<u32>{0u, 65535u}),
                    field(&DepositRecord::m_field, metadata("field", "fields"), StringDomain{128u, false}),
                    field(&DepositRecord::m_material, metadata("material", "materials"), StringDomain{128u, false}),
                    field(
                        &DepositRecord::m_host_material, metadata("host_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &DepositRecord::m_depth_min_normalized, metadata("depth_min_normalized"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &DepositRecord::m_depth_max_normalized, metadata("depth_max_normalized"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &DepositRecord::m_threshold_min, metadata("threshold_min"),
                        FloatDomain{-1.0, 1.0, false, false}),
                    field(
                        &DepositRecord::m_threshold_max, metadata("threshold_max"),
                        FloatDomain{-1.0, 1.0, false, false}),
                    field(
                        &DepositRecord::m_shape, metadata("shape"),
                        EnumDomain<sim::EGenerationDepositShape, 2>{
                            {EnumEntry<sim::EGenerationDepositShape>{sim::EGenerationDepositShape::field, "field"},
                                EnumEntry<sim::EGenerationDepositShape>{sim::EGenerationDepositShape::blob, "blob"}}}),
                    field(
                        &DepositRecord::m_stage, metadata("stage"),
                        EnumDomain<sim::EGenerationDepositStage, 2>{
                            {EnumEntry<sim::EGenerationDepositStage>{sim::EGenerationDepositStage::bulk, "bulk"},
                                EnumEntry<sim::EGenerationDepositStage>{
                                    sim::EGenerationDepositStage::detail, "detail"}}}),
                    field(&DepositRecord::m_blob_count, metadata("blob_count"), Range<u32>{0u, 4096u}),
                    field(
                        &DepositRecord::m_radius_min_m, metadata("radius_min_m", {}, "m"),
                        FloatDomain{0.0, 256.0, false, false}),
                    field(
                        &DepositRecord::m_radius_max_m, metadata("radius_max_m", {}, "m"),
                        FloatDomain{0.0, 256.0, false, false}),
                    field(&DepositRecord::m_stretch_min, metadata("stretch_min"), FloatDomain{0.1, 16.0, false, false}),
                    field(&DepositRecord::m_stretch_max, metadata("stretch_max"), FloatDomain{0.1, 16.0, false, false}),
                    field(&DepositRecord::m_distortion, metadata("distortion"), FloatDomain{0.0, 0.45, false, false}),
                    field(
                        &DepositRecord::m_orientation_min_degrees, metadata("orientation_min_degrees", {}, "degrees"),
                        FloatDomain{0.0, 360.0, false, false}),
                    field(
                        &DepositRecord::m_orientation_max_degrees, metadata("orientation_max_degrees", {}, "degrees"),
                        FloatDomain{0.0, 360.0, false, false}),
                    field(
                        &DepositRecord::m_core_coverage, metadata("core_coverage"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &DepositRecord::m_fringe_coverage, metadata("fringe_coverage"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &DepositRecord::m_sprinkle_probability, metadata("sprinkle_probability"),
                        FloatDomain{0.0, 0.02, false, false})};
            }
        };

        struct FeatureRecord {
            std::string m_display_name{};
            bool m_allow_horizontal_reflection{};
            i32 m_min_x_fine{}, m_min_y_fine{}, m_max_x_fine{}, m_max_y_fine{};
            static constexpr auto describe() {
                return std::tuple{
                    field(&FeatureRecord::m_display_name, metadata("display_name"), StringDomain{256u, false}),
                    field(
                        &FeatureRecord::m_allow_horizontal_reflection, metadata("allow_horizontal_reflection"),
                        boolean_domain),
                    field(
                        &FeatureRecord::m_min_x_fine, metadata("reservation_min_x_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096}),
                    field(
                        &FeatureRecord::m_min_y_fine, metadata("reservation_min_y_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096}),
                    field(
                        &FeatureRecord::m_max_x_fine, metadata("reservation_max_x_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096}),
                    field(
                        &FeatureRecord::m_max_y_fine, metadata("reservation_max_y_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096})};
            }
        };
        struct FeatureOperationRecord {
            u32 m_order{};
            EFeatureAction m_action{};
            EFeatureShape m_shape{};
            i32 m_x_fine{}, m_y_fine{};
            std::optional<i32> m_end_x_fine{}, m_end_y_fine{};
            std::optional<u32> m_width_fine{}, m_height_fine{};
            std::optional<double> m_radius_m{};
            std::string m_material{};
            static constexpr auto describe() {
                return std::tuple{
                    field(&FeatureOperationRecord::m_order, metadata("order"), Range<u32>{0u, 65535u}),
                    field(
                        &FeatureOperationRecord::m_action, metadata("action"),
                        EnumDomain<EFeatureAction, 2>{
                            {EnumEntry<EFeatureAction>{EFeatureAction::paint, "paint"},
                                EnumEntry<EFeatureAction>{EFeatureAction::carve, "carve"}}}),
                    field(
                        &FeatureOperationRecord::m_shape, metadata("shape"),
                        EnumDomain<EFeatureShape, 3>{
                            {EnumEntry<EFeatureShape>{EFeatureShape::rectangle, "rectangle"},
                                EnumEntry<EFeatureShape>{EFeatureShape::ellipse, "ellipse"},
                                EnumEntry<EFeatureShape>{EFeatureShape::capsule, "capsule"}}}),
                    field(
                        &FeatureOperationRecord::m_x_fine, metadata("x_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096}),
                    field(
                        &FeatureOperationRecord::m_y_fine, metadata("y_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096}),
                    field(
                        &FeatureOperationRecord::m_end_x_fine, metadata("end_x_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096}),
                    field(
                        &FeatureOperationRecord::m_end_y_fine, metadata("end_y_fine", {}, "fine cells"),
                        Range<i32>{-4096, 4096}),
                    field(
                        &FeatureOperationRecord::m_width_fine, metadata("width_fine", {}, "fine cells"),
                        Range<u32>{1u, 4096u}),
                    field(
                        &FeatureOperationRecord::m_height_fine, metadata("height_fine", {}, "fine cells"),
                        Range<u32>{1u, 4096u}),
                    field(
                        &FeatureOperationRecord::m_radius_m, metadata("radius_m", {}, "m"),
                        FloatDomain{0.0, 4096.0 / 3.0, false, true}),
                    field(
                        &FeatureOperationRecord::m_material, metadata("material", "materials"),
                        StringDomain{128u, false})};
            }
        };
        struct FeatureMarkerRecord {
            EFeatureMarkerRole m_role{};
            i32 m_x_fine{}, m_y_fine{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &FeatureMarkerRecord::m_role, metadata("role"),
                        EnumDomain<EFeatureMarkerRole, 5>{
                            {EnumEntry<EFeatureMarkerRole>{EFeatureMarkerRole::start, "start"},
                                EnumEntry<EFeatureMarkerRole>{EFeatureMarkerRole::traversal_room, "traversal_room"},
                                EnumEntry<EFeatureMarkerRole>{EFeatureMarkerRole::sealed_pocket, "sealed_pocket"},
                                EnumEntry<EFeatureMarkerRole>{EFeatureMarkerRole::resource_anchor, "resource_anchor"},
                                EnumEntry<EFeatureMarkerRole>{EFeatureMarkerRole::inert, "inert"}}}),
                    field(
                        &FeatureMarkerRecord::m_x_fine, metadata("x_fine", {}, "fine cells"), Range<i32>{-4096, 4096}),
                    field(
                        &FeatureMarkerRecord::m_y_fine, metadata("y_fine", {}, "fine cells"), Range<i32>{-4096, 4096})};
            }
        };

        struct WorldProfileRecord {
            std::string m_display_name{}, m_default_wall_material{};
            std::string m_background_stack{};
            u32 m_boundary_fine{}, m_min_sites{}, m_max_sites{}, m_site_attempts{};
            double m_site_spacing_m{}, m_boundary_warp_amplitude_m{};
            std::string m_boundary_warp_x_field{}, m_boundary_warp_y_field{};
            std::string m_start_biome{}, m_start_feature{}, m_start_marker{};
            u32 m_start_x_fine{}, m_start_y_fine{};
            double m_start_clearance_m{}, m_start_route_width_m{};
            std::optional<std::string> m_surface_field{}, m_bedrock_field{}, m_bedrock_material{};
            double m_surface_elevation_m{}, m_surface_amplitude_m{}, m_bedrock_thickness_m{}, m_bedrock_variation_m{};
            std::optional<double> m_start_depth_m{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &WorldProfileRecord::m_surface_field, metadata("surface_field", "fields"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_bedrock_field, metadata("bedrock_field", "fields"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_bedrock_material, metadata("bedrock_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_surface_elevation_m, metadata("surface_elevation_m", {}, "m"),
                        FloatDomain{0.0, 1365.0, false, false}),
                    field(
                        &WorldProfileRecord::m_surface_amplitude_m, metadata("surface_amplitude_m", {}, "m"),
                        FloatDomain{0.0, 256.0, false, false}),
                    field(
                        &WorldProfileRecord::m_bedrock_thickness_m, metadata("bedrock_thickness_m", {}, "m"),
                        FloatDomain{0.0, 40.0, false, false}),
                    field(
                        &WorldProfileRecord::m_bedrock_variation_m, metadata("bedrock_variation_m", {}, "m"),
                        FloatDomain{0.0, 12.5, false, false}),
                    field(
                        &WorldProfileRecord::m_start_depth_m, metadata("start_depth_m", {}, "m"),
                        FloatDomain{0.0, 1000.0, false, false}),
                    field(&WorldProfileRecord::m_display_name, metadata("display_name"), StringDomain{256u, false}),
                    field(
                        &WorldProfileRecord::m_background_stack, metadata("background_stack", "backgrounds"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_default_wall_material, metadata("default_wall_material", "materials"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_boundary_fine, metadata("boundary_fine", {}, "fine cells"),
                        Range<u32>{1u, 128u}),
                    field(
                        &WorldProfileRecord::m_min_sites, metadata("min_sites"),
                        Range<u32>{1u, static_cast<u32>(kMaxWorldSites)}),
                    field(
                        &WorldProfileRecord::m_max_sites, metadata("max_sites"),
                        Range<u32>{1u, static_cast<u32>(kMaxWorldSites)}),
                    field(&WorldProfileRecord::m_site_attempts, metadata("site_attempts"), Range<u32>{1u, 64u}),
                    field(
                        &WorldProfileRecord::m_site_spacing_m,
                        metadata("site_spacing_m", {}, "m", {}, {}, "Minimum spacing between macro biome sites."),
                        FloatDomain{0.0, 4096.0 / 3.0, false, true}),
                    field(
                        &WorldProfileRecord::m_boundary_warp_amplitude_m,
                        metadata("boundary_warp_amplitude_m", {}, "m"), FloatDomain{0.0, 4096.0 / 3.0, false, false}),
                    field(
                        &WorldProfileRecord::m_boundary_warp_x_field, metadata("boundary_warp_x_field", "fields"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_boundary_warp_y_field, metadata("boundary_warp_y_field", "fields"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_start_biome, metadata("start_biome", "biomes"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_start_feature, metadata("start_feature", "features"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_start_marker, metadata("start_marker", "features"),
                        StringDomain{128u, false}),
                    field(
                        &WorldProfileRecord::m_start_x_fine, metadata("start_x_fine", {}, "fine cells"),
                        Range<u32>{0u, 4095u}),
                    field(
                        &WorldProfileRecord::m_start_y_fine, metadata("start_y_fine", {}, "fine cells"),
                        Range<u32>{0u, 4095u}),
                    field(
                        &WorldProfileRecord::m_start_clearance_m, metadata("start_clearance_m", {}, "m"),
                        FloatDomain{3.0, 4096.0 / 3.0, false, false}),
                    field(
                        &WorldProfileRecord::m_start_route_width_m, metadata("start_route_width_m", {}, "m"),
                        FloatDomain{0.0, 4096.0 / 3.0, false, true})};
            }
        };
        struct WorldBiomeRecord {
            std::string m_biome{};
            double m_weight{};
            u32 m_required_count{};
            double m_depth_min_normalized{}, m_depth_max_normalized{}, m_distance_min_m{}, m_distance_max_m{};
            static constexpr auto describe() {
                return std::tuple{
                    field(&WorldBiomeRecord::m_biome, metadata("biome", "biomes"), StringDomain{128u, false}),
                    field(
                        &WorldBiomeRecord::m_weight,
                        metadata("weight", {}, {}, 0.0, 10.0, "Relative likelihood among competing choices."),
                        FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}),
                    field(
                        &WorldBiomeRecord::m_required_count, metadata("required_count"),
                        Range<u32>{0u, static_cast<u32>(kMaxWorldSites)}),
                    field(
                        &WorldBiomeRecord::m_depth_min_normalized, metadata("depth_min_normalized"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &WorldBiomeRecord::m_depth_max_normalized, metadata("depth_max_normalized"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &WorldBiomeRecord::m_distance_min_m, metadata("distance_min_m", {}, "m"),
                        FloatDomain{0.0, 2048.0, false, false}),
                    field(
                        &WorldBiomeRecord::m_distance_max_m, metadata("distance_max_m", {}, "m"),
                        FloatDomain{0.0, 2048.0, false, false})};
            }
        };
        struct StartResourceRecord {
            std::string m_marker{}, m_material{};
            u32 m_minimum_fine_cells{};
            double m_max_distance_m{};
            static constexpr auto describe() {
                return std::tuple{
                    field(&StartResourceRecord::m_marker, metadata("marker", "features"), StringDomain{128u, false}),
                    field(
                        &StartResourceRecord::m_material, metadata("material", "materials"), StringDomain{128u, false}),
                    field(
                        &StartResourceRecord::m_minimum_fine_cells, metadata("minimum_fine_cells", {}, "fine cells"),
                        Range<u32>{1u, 65536u}),
                    field(
                        &StartResourceRecord::m_max_distance_m, metadata("max_distance_m", {}, "m"),
                        FloatDomain{0.0, 2048.0, false, true})};
            }
        };
        struct WorldRuleHeader {
            EWorldRuleKind m_kind{};
            EWorldRulePhase m_phase{};
            u32 m_order{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &WorldRuleHeader::m_kind,
                        metadata(
                            "kind", {}, {}, {}, {},
                            "Rule kind fixes its required schema; start from a profile containing the desired rule kind.",
                            true),
                        EnumDomain<EWorldRuleKind, 6>{
                            {EnumEntry<EWorldRuleKind>{EWorldRuleKind::threshold_cave, "threshold_cave"},
                                EnumEntry<EWorldRuleKind>{EWorldRuleKind::carve_worm, "carve_worm"},
                                EnumEntry<EWorldRuleKind>{EWorldRuleKind::vein_worm, "vein_worm"},
                                EnumEntry<EWorldRuleKind>{EWorldRuleKind::connect_rooms, "connect_rooms"},
                                EnumEntry<EWorldRuleKind>{EWorldRuleKind::feature, "feature"},
                                EnumEntry<EWorldRuleKind>{EWorldRuleKind::matter_fill, "matter_fill"}}}),
                    field(
                        &WorldRuleHeader::m_phase, metadata("phase"),
                        EnumDomain<EWorldRulePhase, 4>{
                            {EnumEntry<EWorldRulePhase>{EWorldRulePhase::deposit, "deposit"},
                                EnumEntry<EWorldRulePhase>{EWorldRulePhase::carve, "carve"},
                                EnumEntry<EWorldRulePhase>{EWorldRulePhase::stamp, "stamp"},
                                EnumEntry<EWorldRulePhase>{EWorldRulePhase::fill, "fill"}}}),
                    field(&WorldRuleHeader::m_order, metadata("order"), Range<u32>{0u, 65535u})};
            }
        };
        struct WorldCaveRecord : WorldRuleHeader {
            std::string m_biome{}, m_field{};
            double m_depth_min_normalized{}, m_depth_max_normalized{}, m_threshold_min{}, m_threshold_max{};
            static constexpr auto describe() {
                return std::tuple_cat(
                    WorldRuleHeader::describe(),
                    std::tuple{
                        field(&WorldCaveRecord::m_biome, metadata("biome", "biomes"), StringDomain{128u, false}),
                        field(&WorldCaveRecord::m_field, metadata("field", "fields"), StringDomain{128u, false}),
                        field(
                            &WorldCaveRecord::m_depth_min_normalized, metadata("depth_min_normalized"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldCaveRecord::m_depth_max_normalized, metadata("depth_max_normalized"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldCaveRecord::m_threshold_min, metadata("threshold_min"),
                            FloatDomain{-1.0, 1.0, false, false}),
                        field(
                            &WorldCaveRecord::m_threshold_max, metadata("threshold_max"),
                            FloatDomain{-1.0, 1.0, false, false})});
            }
        };
        struct WorldWormRecord : WorldRuleHeader {
            std::string m_biome{}, m_material{}, m_host_material{};
            u32 m_count{};
            double m_depth_min_normalized{}, m_depth_max_normalized{};
            double m_length_min_m{}, m_length_max_m{}, m_radius_min_m{}, m_radius_max_m{}, m_turn_probability{};
            double m_turn_smoothing{}, m_radius_variation{}, m_chamber_probability{}, m_chamber_scale{1.0};
            static constexpr auto describe() {
                return std::tuple_cat(
                    WorldRuleHeader::describe(),
                    std::tuple{
                        field(&WorldWormRecord::m_biome, metadata("biome", "biomes"), StringDomain{128u, false}),
                        field(
                            &WorldWormRecord::m_material, metadata("material", "materials"), StringDomain{128u, false}),
                        field(
                            &WorldWormRecord::m_host_material, metadata("host_material", "materials"),
                            StringDomain{128u, false}),
                        field(&WorldWormRecord::m_count, metadata("count"), Range<u32>{1u, 1024u}),
                        field(
                            &WorldWormRecord::m_depth_min_normalized, metadata("depth_min_normalized"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldWormRecord::m_depth_max_normalized, metadata("depth_max_normalized"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldWormRecord::m_length_min_m, metadata("length_min_m", {}, "m"),
                            FloatDomain{0.0, 4096.0 / 3.0, false, true}),
                        field(
                            &WorldWormRecord::m_length_max_m, metadata("length_max_m", {}, "m"),
                            FloatDomain{0.0, 4096.0 / 3.0, false, true}),
                        field(
                            &WorldWormRecord::m_radius_min_m, metadata("radius_min_m", {}, "m"),
                            FloatDomain{0.0, 64.0, false, true}),
                        field(
                            &WorldWormRecord::m_radius_max_m, metadata("radius_max_m", {}, "m"),
                            FloatDomain{0.0, 64.0, false, true}),
                        field(
                            &WorldWormRecord::m_turn_probability, metadata("turn_probability"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldWormRecord::m_turn_smoothing, metadata("turn_smoothing"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldWormRecord::m_radius_variation, metadata("radius_variation"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldWormRecord::m_chamber_probability, metadata("chamber_probability"),
                            FloatDomain{0.0, 0.1, false, false}),
                        field(
                            &WorldWormRecord::m_chamber_scale, metadata("chamber_scale"),
                            FloatDomain{1.0, 4.0, false, false})});
            }
        };
        struct WorldConnectionRecord : WorldRuleHeader {
            double m_radius_m{};
            u32 m_extra_links{};
            u32 m_natural_anchors{}, m_wells{}, m_branches{};
            double m_roughness_m{};
            static constexpr auto describe() {
                return std::tuple_cat(
                    WorldRuleHeader::describe(),
                    std::tuple{
                        field(
                            &WorldConnectionRecord::m_natural_anchors, metadata("natural_anchors"),
                            Range<u32>{0u, 128u}),
                        field(&WorldConnectionRecord::m_wells, metadata("wells"), Range<u32>{0u, 32u}),
                        field(&WorldConnectionRecord::m_branches, metadata("branches"), Range<u32>{0u, 64u}),
                        field(
                            &WorldConnectionRecord::m_roughness_m, metadata("roughness_m", {}, "m"),
                            FloatDomain{0.0, 8.0, false, false}),
                        field(
                            &WorldConnectionRecord::m_radius_m, metadata("radius_m", {}, "m"),
                            FloatDomain{0.0, 64.0, false, true}),
                        field(&WorldConnectionRecord::m_extra_links, metadata("extra_links"), Range<u32>{0u, 64u})});
            }
        };
        struct WorldFeatureRecord : WorldRuleHeader {
            std::string m_feature{}, m_biome{};
            double m_depth_min_normalized{}, m_depth_max_normalized{}, m_spacing_m{};
            u32 m_min_count{}, m_max_count{}, m_attempts{}, m_priority{};
            bool m_reflect{};
            static constexpr auto describe() {
                return std::tuple_cat(
                    WorldRuleHeader::describe(),
                    std::tuple{
                        field(
                            &WorldFeatureRecord::m_feature, metadata("feature", "features"), StringDomain{128u, false}),
                        field(&WorldFeatureRecord::m_biome, metadata("biome", "biomes"), StringDomain{128u, false}),
                        field(
                            &WorldFeatureRecord::m_depth_min_normalized, metadata("depth_min_normalized"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldFeatureRecord::m_depth_max_normalized, metadata("depth_max_normalized"),
                            FloatDomain{0.0, 1.0, false, false}),
                        field(
                            &WorldFeatureRecord::m_spacing_m, metadata("spacing_m", {}, "m"),
                            FloatDomain{0.0, 4096.0 / 3.0, false, true}),
                        field(&WorldFeatureRecord::m_min_count, metadata("min_count"), Range<u32>{0u, 1024u}),
                        field(&WorldFeatureRecord::m_max_count, metadata("max_count"), Range<u32>{1u, 1024u}),
                        field(&WorldFeatureRecord::m_attempts, metadata("attempts"), Range<u32>{1u, 4096u}),
                        field(&WorldFeatureRecord::m_priority, metadata("priority"), Range<u32>{0u, 65535u}),
                        field(&WorldFeatureRecord::m_reflect, metadata("reflect"), boolean_domain)});
            }
        };
        struct WorldMatterRecord : WorldRuleHeader {
            std::string m_feature_rule{}, m_marker{}, m_material{};
            double m_temperature_k{}, m_bulk_density_kg_m3{};
            std::optional<double> m_fill_height_m{};
            static constexpr auto describe() {
                return std::tuple_cat(
                    WorldRuleHeader::describe(),
                    std::tuple{
                        field(
                            &WorldMatterRecord::m_feature_rule, metadata("feature_rule", "worlds"),
                            StringDomain{128u, false}),
                        field(&WorldMatterRecord::m_marker, metadata("marker", "features"), StringDomain{128u, false}),
                        field(
                            &WorldMatterRecord::m_material, metadata("material", "materials"),
                            StringDomain{128u, false}),
                        field(
                            &WorldMatterRecord::m_temperature_k,
                            metadata("temperature_k", {}, "K", 200.0, 1000.0, "Initial matter temperature."),
                            FloatDomain{0.0, static_cast<double>((std::numeric_limits<float>::max)()), false, true}),
                        field(
                            &WorldMatterRecord::m_bulk_density_kg_m3,
                            metadata(
                                "bulk_density_kg_m3", {}, "kg/m3", 0.0, 10000.0,
                                "Initial matter mass per cubic metre."),
                            FloatDomain{0.0, 1.0e9, false, true}),
                        field(
                            &WorldMatterRecord::m_fill_height_m, metadata("fill_height_m", {}, "m"),
                            FloatDomain{-4096.0 / 3.0, 4096.0 / 3.0, false, false})});
            }
        };

        struct MaterialRecord {
            u16 m_numeric_id{0};
            std::string m_display_name{};
            EMaterialPhase m_phase{};
            EMaterialRepresentation m_representation{};
            std::string m_color_rgba{};
            std::optional<std::string> m_albedo_texture{};
            std::optional<double> m_texture_period_m{};
            std::optional<bool> m_diggable{};
            std::optional<double> m_roundness{};
            double m_reference_density_kg_m3{0.0};
            double m_metallic{}, m_roughness{1.0}, m_emissive_r{}, m_emissive_g{}, m_emissive_b{};
            double m_emissive_strength{1.0}, m_ior{1.5}, m_normal_scale{1.0}, m_refraction_depth_m{};
            EMaterialAlphaMode m_alpha_mode{EMaterialAlphaMode::opaque};
            EMaterialTextureSampling m_texture_sampling{EMaterialTextureSampling::stochastic};
            std::optional<std::string> m_normal_texture{};

            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &MaterialRecord::m_numeric_id,
                        metadata("numeric_id", {}, {}, {}, {}, "Material identity is fixed after creation.", true),
                        Range<u16>{1u, 65534u}),
                    field(&MaterialRecord::m_display_name, metadata("display_name"), StringDomain{256u, false}),
                    field(
                        &MaterialRecord::m_phase, metadata("phase"),
                        EnumDomain<EMaterialPhase, 3>{
                            {EnumEntry<EMaterialPhase>{EMaterialPhase::solid, "solid"},
                                EnumEntry<EMaterialPhase>{EMaterialPhase::liquid, "liquid"},
                                EnumEntry<EMaterialPhase>{EMaterialPhase::gas, "gas"}}}),
                    field(
                        &MaterialRecord::m_representation, metadata("representation"),
                        EnumDomain<EMaterialRepresentation, 2>{
                            {EnumEntry<EMaterialRepresentation>{EMaterialRepresentation::terrain, "terrain"},
                                EnumEntry<EMaterialRepresentation>{EMaterialRepresentation::particle, "particle"}}}),
                    field(&MaterialRecord::m_color_rgba, metadata("color_rgba"), StringDomain{8u, false}),
                    field(&MaterialRecord::m_albedo_texture, metadata("albedo_texture"), StringDomain{1024u, false}),
                    field(
                        &MaterialRecord::m_texture_period_m,
                        metadata(
                            "texture_period_m", {}, "m", 0.125, 16.0, "World distance covered by one texture repeat."),
                        // Float UV arithmetic must remain finite over the entire world.
                        FloatDomain{
                            4096.0 / static_cast<double>((std::numeric_limits<float>::max)()),
                            static_cast<double>((std::numeric_limits<float>::max)()), false, false}),
                    field(&MaterialRecord::m_diggable, metadata("diggable"), boolean_domain),
                    field(&MaterialRecord::m_roundness, metadata("roundness"), FloatDomain{0.0, 1.0, false, false}),
                    field(&MaterialRecord::m_metallic, metadata("metallic"), FloatDomain{0.0, 1.0, false, false}),
                    field(&MaterialRecord::m_roughness, metadata("roughness"), FloatDomain{0.0, 1.0, false, false}),
                    field(&MaterialRecord::m_emissive_r, metadata("emissive_r"), FloatDomain{0.0, 1.0, false, false}),
                    field(&MaterialRecord::m_emissive_g, metadata("emissive_g"), FloatDomain{0.0, 1.0, false, false}),
                    field(&MaterialRecord::m_emissive_b, metadata("emissive_b"), FloatDomain{0.0, 1.0, false, false}),
                    field(
                        &MaterialRecord::m_emissive_strength, metadata("emissive_strength", {}, {}, 0.0, 16.0),
                        FloatDomain{0.0, static_cast<double>((std::numeric_limits<float>::max)()), false, false}),
                    field(&MaterialRecord::m_ior, metadata("ior"), FloatDomain{1.0, 3.0, false, false}),
                    field(
                        &MaterialRecord::m_normal_scale, metadata("normal_scale"), FloatDomain{0.0, 4.0, false, false}),
                    field(
                        &MaterialRecord::m_refraction_depth_m, metadata("refraction_depth_m", {}, "m"),
                        FloatDomain{0.0, 1.0, false, false}),
                    field(&MaterialRecord::m_normal_texture, metadata("normal_texture"), StringDomain{1024u, false}),
                    field(
                        &MaterialRecord::m_alpha_mode, metadata("alpha_mode"),
                        EnumDomain<EMaterialAlphaMode, 2>{
                            {EnumEntry<EMaterialAlphaMode>{EMaterialAlphaMode::opaque, "opaque"},
                                EnumEntry<EMaterialAlphaMode>{EMaterialAlphaMode::blend, "blend"}}}),
                    field(
                        &MaterialRecord::m_texture_sampling, metadata("texture_sampling"),
                        EnumDomain<EMaterialTextureSampling, 2>{
                            {EnumEntry<EMaterialTextureSampling>{EMaterialTextureSampling::stochastic, "stochastic"},
                                EnumEntry<EMaterialTextureSampling>{EMaterialTextureSampling::repeat, "repeat"}}}),
                    field(
                        &MaterialRecord::m_reference_density_kg_m3,
                        metadata(
                            "reference_density_kg_m3", {}, "kg/m3", 0.0, 10000.0,
                            "Reference material mass per cubic metre."),
                        FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}),
                };
            }
        };

        constexpr std::array<std::string_view, kContentDocumentCount> kContentCollections{
            "materials", "biomes", "fields", "features", "worlds", "backgrounds"};
        template<typename Record>
        std::optional<ContentPropertyDescriptor> recordPropertyDescriptor(const std::string_view property) {
            static constexpr auto schema = Record::describe();
            std::optional<ContentPropertyDescriptor> result;
            const auto inspect = [&](const auto &member) {
                if (member.m_metadata.m_name == property) {
                    ContentPropertyDescriptor descriptor;
                    static_cast<config::PropertyDescriptor &>(descriptor) = config::describeProperty(member);
                    for (std::size_t index = 0u; index < kContentCollections.size(); ++index) {
                        if (descriptor.m_reference == kContentCollections[index]) {
                            descriptor.m_reference_category = static_cast<EContentDocument>(index);
                            break;
                        }
                    }
                    result = std::move(descriptor);
                }
            };
            std::apply([&](const auto &...member) { (inspect(member), ...); }, schema);
            return result;
        }
        std::string_view documentText(const Document &document, const std::string_view key) noexcept {
            const auto entry = std::ranges::find(document.m_entries, key, &Entry::m_key);
            return entry != document.m_entries.end() and entry->m_value.m_kind == EFileKind::text
                       ? std::string_view{entry->m_value.m_text}
                       : std::string_view{};
        }
        std::optional<EWorldRuleKind> documentRuleKind(const Document &document, const std::string_view prefix) {
            EWorldRuleKind kind{};
            constexpr auto schema = WorldRuleHeader::describe();
            if (std::get<0u>(schema).m_constraint.parse(documentText(document, std::string{prefix} + "kind"), kind)) {
                return std::nullopt;
            }
            return kind;
        }
        struct NestedProperty {
            std::string_view m_collection, m_key, m_property;
        };
        std::optional<NestedProperty> nestedProperty(const std::string_view property) noexcept {
            const auto first = property.find('.');
            if (first == std::string_view::npos) {
                return std::nullopt;
            }
            const auto second = property.find('.', first + 1u);
            if (second == std::string_view::npos or second == first + 1u or second + 1u == property.size()) {
                return std::nullopt;
            }
            return NestedProperty{
                property.substr(0u, first), property.substr(first + 1u, second - first - 1u),
                property.substr(second + 1u)};
        }
        Array<std::string> recordKeys(const Document &document, const std::string_view prefix) {
            Array<std::string> result;
            for (const auto &entry: document.m_entries) {
                if (entry.m_key.starts_with(prefix)) {
                    const auto remainder = std::string_view{entry.m_key}.substr(prefix.size());
                    const auto end = remainder.find('.');
                    if (end != std::string_view::npos and end != 0u) {
                        result.emplace_back(remainder.substr(0u, end));
                    }
                }
            }
            std::ranges::sort(result);
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        [[nodiscard]] std::error_code reject(
            Diagnostics &diagnostics, const std::string_view location, const std::string_view key,
            const std::string_view reason, const EConfigErrc code = EConfigErrc::cross_field_invalid) {
            const auto error = config::make_error_code(code);
            diagnostics.add(std::string{location}, std::string{key}, std::string{reason}, error);
            return error;
        }

        [[nodiscard]] bool validName(const std::string_view name) noexcept {
            return not name.empty() and
                   name.front() >= 'a' and
                   name.front() <= 'z' and
                   std::ranges::all_of(name, [](const char c) {
                       return (c >= 'a' and c <= 'z') or (c >= '0' and c <= '9') or c == '_';
                   });
        }

        [[nodiscard]] bool validTexturePath(const std::string_view path) noexcept {
            if (path.empty() or path.front() == '/' or path.find_first_of("\\:\0", 0u, 3u) != std::string_view::npos) {
                return false;
            }
            std::size_t start = 0u;
            while (start < path.size()) {
                const auto end = path.find('/', start);
                const auto segment = path.substr(
                    start, end == std::string_view::npos ? path.size() - start : end - start);
                if (segment.empty() or segment == "." or segment == "..") {
                    return false;
                }
                if (end == std::string_view::npos) {
                    return true;
                }
                start = end + 1u;
            }
            return false;
        }

        [[nodiscard]] std::error_code requireFields(
            const Document &document, const std::string_view origin, Diagnostics &diagnostics,
            const std::span<const std::string_view> required) {
            for (const auto name: required) {
                if (std::ranges::none_of(document.m_entries, [&](const Entry &entry) { return entry.m_key == name; })) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, origin, name, "Required content field is missing",
                                           EConfigErrc::missing_value));
                }
            }
            return {};
        }

        [[nodiscard]] Expected<MaterialDefinition> compileMaterial(
            const std::string &key, const Document &document, const std::string_view origin, Diagnostics &diagnostics) {
            constexpr std::string_view required[] = {
                "numeric_id", "display_name", "phase", "representation", "color_rgba", "reference_density_kg_m3"};
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, requireFields(document, origin, diagnostics, required));
            const Layer layers[] = {toLayer(document, std::string{origin})};
            auto resolved = resolve<MaterialRecord>(layers, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved);
            const auto &record = resolved->m_value;
            const auto location = [&](const std::string_view field) -> std::string_view {
                const auto found = std::ranges::find(document.m_entries, field, &Entry::m_key);
                return found == document.m_entries.end() ? origin : std::string_view{found->m_location};
            };

            u32 color = 0u;
            const auto [end, error] = std::from_chars(
                record.m_color_rgba.data(), record.m_color_rgba.data() + record.m_color_rgba.size(), color, 16);
            if (record.m_color_rgba.size() != 8u or error != std::errc{} or end != record.m_color_rgba.data() + 8u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location("color_rgba"), "color_rgba",
                                       "Expected exactly eight hexadecimal RRGGBBAA digits"));
            }
            if (record.m_representation == EMaterialRepresentation::terrain) {
                if (record.m_phase != EMaterialPhase::solid) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(diagnostics, location("phase"), "phase", "Terrain requires solid phase"));
                }
                for (const auto name: {"albedo_texture", "texture_period_m", "diggable"}) {
                    if (std::ranges::none_of(
                            document.m_entries, [&](const Entry &entry) { return entry.m_key == name; })) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyContent, reject(
                                               diagnostics, origin, name, "Required terrain field is missing",
                                               EConfigErrc::missing_value));
                    }
                }
            } else if (record.m_diggable.has_value() or record.m_roundness) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, "representation",
                                       "Particle materials cannot declare terrain-only fields"));
            }
            if ((record.m_albedo_texture or record.m_normal_texture) and not record.m_texture_period_m) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(diagnostics, origin, "texture_period_m", "Textured materials require a world-space period"));
            }
            if (record.m_normal_texture and not validTexturePath(*record.m_normal_texture)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location("normal_texture"), "normal_texture",
                                       "Expected a portable pack-relative path without dot segments"));
            }
            if (record.m_albedo_texture and not validTexturePath(*record.m_albedo_texture)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location("albedo_texture"), "albedo_texture",
                                       "Expected a portable pack-relative path without dot segments"));
            }
            return MaterialDefinition{
                key, record.m_numeric_id, record.m_display_name, record.m_phase, record.m_representation, color,
                record.m_albedo_texture, record.m_texture_period_m, record.m_diggable, record.m_reference_density_kg_m3,
                record.m_albedo_texture ? std::string{location("albedo_texture")} : std::string{},
                record.m_roundness.value_or(1.0), record.m_metallic, record.m_roughness,
                {record.m_emissive_r, record.m_emissive_g, record.m_emissive_b}, record.m_emissive_strength,
                record.m_alpha_mode, record.m_ior, record.m_normal_texture, record.m_normal_scale,
                record.m_refraction_depth_m,
                record.m_normal_texture ? std::string{location("normal_texture")} : std::string{},
                record.m_texture_sampling};
        }

        [[nodiscard]] Expected<std::map<std::string, Document, std::less<>>> groupRecords(
            const std::string_view bytes, const std::string_view origin, const std::string_view collection,
            const std::size_t record_limit, Diagnostics &diagnostics) {
            if (bytes.size() > kMaxMaterialDocumentBytes) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(diagnostics, origin, "", "Content document exceeds 1 MiB", EConfigErrc::out_of_range));
            }
            auto document = decode(bytes, origin, diagnostics, EDecodeMode::records);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
            std::ranges::sort(document->m_entries, {}, &Entry::m_key);
            const auto version = std::ranges::find(document->m_entries, "schema_version", &Entry::m_key);
            if (version == document->m_entries.end()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, "schema_version", "Required schema version is missing",
                                       EConfigErrc::missing_value));
            }
            if (version->m_value.m_kind != EFileKind::integer or version->m_value.m_integer != kContentSchemaVersion) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, version->m_location, "schema_version",
                                       "Only content schema version 2 is supported"));
            }

            std::map<std::string, Document, std::less<>> groups{};
            const std::string prefix = std::string{collection} + ".";
            for (const auto &entry: document->m_entries) {
                if (entry.m_key == "schema_version") {
                    continue;
                }
                const std::string_view full = entry.m_key;
                const auto separator = full.find('.', prefix.size());
                if (not full.starts_with(prefix) or
                    separator == std::string_view::npos or
                    not validName(full.substr(prefix.size(), separator - prefix.size()))) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, entry.m_location, full,
                                           "Expected " + prefix + "<record>.<field> with a lowercase record name",
                                           EConfigErrc::unknown_property));
                }
                const std::string key{full.substr(prefix.size(), separator - prefix.size())};
                if (not groups.contains(key) and groups.size() == record_limit) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, entry.m_location, full, "Record count exceeds " + std::to_string(record_limit),
                            EConfigErrc::out_of_range));
                }
                Entry field = entry;
                field.m_key = full.substr(separator + 1u);
                groups[key].m_entries.push_back(std::move(field));
            }
            if (groups.empty()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, collection, "At least one record is required",
                                       EConfigErrc::missing_value));
            }
            return groups;
        }

        [[nodiscard]] Expected<FieldDefinition> compileField(
            const std::string &key, const Document &document, const std::string_view origin, Diagnostics &diagnostics) {
            constexpr std::string_view required[] = {"algorithm", "frequency_per_metre"};
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, requireFields(document, origin, diagnostics, required));
            const Layer layers[] = {toLayer(document, std::string{origin})};
            auto resolved = resolve<FieldRecord>(layers, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved);
            const auto &record = resolved->m_value;
            const sim::NoiseFieldDesc noise{
                record.m_algorithm, record.m_fractal, record.m_frequency_per_metre, record.m_octaves,
                record.m_lacunarity, record.m_gain, record.m_warp_amplitude_m, record.m_warp_frequency_per_metre,
                record.m_axis_scale_x, record.m_axis_scale_y, record.m_rotation_degrees};
            if (const auto error = sim::validateNoiseFieldDescription(noise)) {
                // Translate the joint engine-domain constraint into authoring
                // diagnostics; preserve the record's source location.
                const auto frequency = std::ranges::find(document.m_entries, "frequency_per_metre", &Entry::m_key);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, frequency->m_location, "frequency_per_metre",
                        "Frequency/octaves/lacunarity/warp exceed the supported noise coordinate domain"));
            }
            return FieldDefinition{key, noise};
        }

        void appendDiagnostics(Diagnostics &to, const Diagnostics &from, const std::string_view prefix) {
            for (const auto &diagnostic: from.entries()) {
                to.add(
                    diagnostic.m_location, std::string{prefix} + diagnostic.m_property, diagnostic.m_message,
                    diagnostic.m_code, diagnostic.m_alias);
            }
            to.addOmitted(from.omittedCount());
        }

        [[nodiscard]] std::string_view fieldLocation(
            const Document &document, const std::string_view field, const std::string_view origin) {
            const auto found = std::ranges::find(document.m_entries, field, &Entry::m_key);
            return found == document.m_entries.end() ? origin : std::string_view{found->m_location};
        }

        template<class Record>
        [[nodiscard]] Expected<Record> resolveRecord(
            const Document &document, const std::span<const std::string_view> required, const std::string_view origin,
            Diagnostics &diagnostics, const std::string_view prefix) {
            Diagnostics local{64u};
            const auto presence = requireFields(document, origin, local, required);
            if (presence) {
                appendDiagnostics(diagnostics, local, prefix);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, presence);
            }
            const Layer layers[] = {toLayer(document, std::string{origin})};
            auto resolved = resolve<Record>(layers, local);
            appendDiagnostics(diagnostics, local, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved);
            return std::move(resolved->m_value);
        }

        struct ChildRecords {
            Document m_base{};
            std::map<std::string, Document, std::less<>> m_children{};
        };

        [[nodiscard]] Expected<ChildRecords> splitChildren(
            const Document &document, const std::string_view collection, const std::size_t limit,
            const std::string_view qualified_prefix, Diagnostics &diagnostics) {
            ChildRecords result{};
            const std::string prefix = std::string{collection} + ".";
            for (const auto &entry: document.m_entries) {
                if (not entry.m_key.starts_with(prefix)) {
                    result.m_base.m_entries.push_back(entry);
                    continue;
                }
                const std::string_view suffix = std::string_view{entry.m_key}.substr(prefix.size());
                const auto separator = suffix.find('.');
                if (separator == std::string_view::npos or not validName(suffix.substr(0u, separator))) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, entry.m_location, std::string{qualified_prefix} + entry.m_key,
                                           "Expected a named child record and field", EConfigErrc::unknown_property));
                }
                const std::string key{suffix.substr(0u, separator)};
                if (not result.m_children.contains(key) and result.m_children.size() == limit) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, entry.m_location, std::string{qualified_prefix} + entry.m_key,
                                           "Child record limit exceeded", EConfigErrc::out_of_range));
                }
                Entry field_entry = entry;
                field_entry.m_key = suffix.substr(separator + 1u);
                result.m_children[key].m_entries.push_back(std::move(field_entry));
            }
            return result;
        }

        [[nodiscard]] std::error_code requireFieldReference(
            const Document &document, const std::string_view property, const std::string_view key,
            const std::span<const FieldDefinition> fields, const std::string_view origin, const std::string_view prefix,
            Diagnostics &diagnostics) {
            if (std::ranges::none_of(fields, [&](const auto &field) { return field.m_key == key; })) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(document, property, origin),
                        std::string{prefix} + std::string{property}, "Unknown field reference: " + std::string{key}));
            }
            return {};
        }

        [[nodiscard]] Expected<u16> terrainReference(
            const Document &document, const std::string_view property, const std::string_view key,
            const ContentCatalog &catalog, const std::string_view origin, const std::string_view prefix,
            Diagnostics &diagnostics) {
            const auto *const material = catalog.findMaterial(key);
            if (not material or material->m_representation != EMaterialRepresentation::terrain) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(document, property, origin),
                                       std::string{prefix} + std::string{property},
                                       "Expected terrain material reference: " + std::string{key}));
            }
            return material->m_numeric_id;
        }

        [[nodiscard]] Expected<BiomeCompositionBand> compileComposition(
            const std::string &key, const Document &document, const ContentCatalog &materials,
            const std::span<const FieldDefinition> fields, const std::string_view origin, const std::string &prefix,
            Diagnostics &diagnostics) {
            auto split = splitChildren(document, "materials", kMaxCompositionMaterials, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, split);
            constexpr std::string_view required[] = {"order", "depth_min_normalized", "depth_max_normalized", "field"};
            auto record = resolveRecord<CompositionRecord>(split->m_base, required, origin, diagnostics, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                requireFieldReference(split->m_base, "field", record->m_field, fields, origin, prefix, diagnostics));
            if (record->m_depth_min_normalized >= record->m_depth_max_normalized) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(split->m_base, "depth_max_normalized", origin),
                        prefix + "depth_max_normalized", "Composition requires a nonempty increasing depth interval"));
            }
            if (split->m_children.empty()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, prefix + "materials",
                                       "Composition requires named material choices", EConfigErrc::missing_value));
            }
            BiomeCompositionBand result{
                key, record->m_order, record->m_depth_min_normalized, record->m_depth_max_normalized,
                std::move(record->m_field), {}, 0.0, record->m_bed_period_m, record->m_bed_warp_m, record->m_bed_dip};
            constexpr std::string_view choice_required[] = {"material", "weight"};
            for (const auto &[choice_key, choice_document]: split->m_children) {
                const std::string choice_prefix = prefix + "materials." + choice_key + ".";
                auto choice = resolveRecord<WeightedMaterialRecord>(
                    choice_document, choice_required, origin, diagnostics, choice_prefix);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, choice);
                // Vacuum is an empty terrain seed, never a particle material.
                auto id = choice->m_material == "vacuum" ? Expected<u16>{u16{0}}
                                                         : terrainReference(
                                                               choice_document, "material", choice->m_material,
                                                               materials, origin, choice_prefix, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, id);
                const double total = result.m_total_weight + choice->m_weight;
                if (not std::isfinite(total) or total == result.m_total_weight) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(choice_document, "weight", origin), choice_prefix + "weight",
                            "Cumulative weight must be finite and each choice must have a representable interval"));
                }
                result.m_total_weight = total;
                result.m_materials.push_back({choice_key, *id, choice->m_weight});
            }
            if (std::ranges::none_of(
                    result.m_materials, [](const auto &choice) { return choice.m_material_id != 0u; })) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, prefix + "materials",
                                       "Composition requires a terrain solid for the protected feature substrate"));
            }
            double cumulative = 0.0;
            double previous = 0.0;
            for (const auto &choice: result.m_materials) {
                cumulative += choice.m_weight;
                const double fraction = cumulative / result.m_total_weight;
                if (fraction <= previous) {
                    const auto &choice_document = split->m_children.at(choice.m_key);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, fieldLocation(choice_document, "weight", origin),
                                           prefix + "materials." + choice.m_key + ".weight",
                                           "Normalized cumulative weight interval is not representable"));
                }
                previous = fraction;
            }
            return result;
        }

        [[nodiscard]] Expected<BiomeThresholdDeposit> compileDeposit(
            const std::string &key, const Document &document, const ContentCatalog &materials,
            const std::span<const FieldDefinition> fields, const std::string_view origin, const std::string &prefix,
            Diagnostics &diagnostics) {
            constexpr std::string_view required[] = {
                "kind", "order", "field", "material", "host_material", "depth_min_normalized", "depth_max_normalized"};
            auto record = resolveRecord<DepositRecord>(document, required, origin, diagnostics, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                requireFieldReference(document, "field", record->m_field, fields, origin, prefix, diagnostics));
            auto material = terrainReference(
                document, "material", record->m_material, materials, origin, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, material);
            auto host = terrainReference(
                document, "host_material", record->m_host_material, materials, origin, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, host);
            if (record->m_depth_min_normalized >= record->m_depth_max_normalized) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(document, "depth_max_normalized", origin),
                        prefix + "depth_max_normalized", "Deposit depth interval must be nonempty and increasing"));
            }
            if (record->m_threshold_min >= record->m_threshold_max) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(document, "threshold_max", origin), prefix + "threshold_max",
                        "Deposit threshold interval must be nonempty and increasing"));
            }
            if (record->m_shape == sim::EGenerationDepositShape::field) {
                for (const std::string_view property: {"threshold_min", "threshold_max"}) {
                    if (std::ranges::none_of(
                            document.m_entries, [&](const Entry &entry) { return entry.m_key == property; })) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyContent,
                            reject(
                                diagnostics, fieldLocation(document, property, origin), prefix + std::string{property},
                                "Field deposits require explicit threshold bounds", EConfigErrc::missing_value));
                    }
                }
                if (record->m_blob_count != 0u or
                    record->m_radius_min_m != 0.0 or
                    record->m_radius_max_m != 0.0 or
                    record->m_stretch_min != 1.0 or
                    record->m_stretch_max != 1.0 or
                    record->m_distortion != 0.0 or
                    record->m_orientation_min_degrees != 0.0 or
                    record->m_orientation_max_degrees != 360.0 or
                    record->m_core_coverage != 1.0 or
                    record->m_fringe_coverage != 1.0 or
                    record->m_sprinkle_probability != 0.0) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(diagnostics, origin, prefix + "shape", "Field deposits require neutral blob controls"));
                }
            } else {
                if (record->m_blob_count == 0u or
                    record->m_radius_min_m <= 0.0 or
                    record->m_radius_max_m < record->m_radius_min_m or
                    record->m_stretch_max < record->m_stretch_min or
                    record->m_orientation_max_degrees < record->m_orientation_min_degrees or
                    record->m_core_coverage < record->m_fringe_coverage or
                    record->m_radius_max_m *
                            (std::max) (1.0, record->m_stretch_max) *
                            (1.0 + record->m_distortion) *
                            1.2 >
                        4096.0 / 3.0 or
                    (record->m_stage == sim::EGenerationDepositStage::bulk and record->m_sprinkle_probability != 0.0)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, origin, prefix + "shape",
                            "Blob deposits require a positive count and ordered positive radius/stretch/density ranges; sprinkle requires detail stage"));
                }
            }
            return BiomeThresholdDeposit{
                key, record->m_order, std::move(record->m_field), *material, *host, record->m_depth_min_normalized,
                record->m_depth_max_normalized, record->m_threshold_min, record->m_threshold_max, record->m_shape,
                record->m_stage, record->m_blob_count, record->m_radius_min_m, record->m_radius_max_m,
                record->m_stretch_min, record->m_stretch_max, record->m_distortion, record->m_core_coverage,
                record->m_fringe_coverage, record->m_sprinkle_probability, record->m_orientation_min_degrees,
                record->m_orientation_max_degrees};
        }

        [[nodiscard]] std::error_code conditionalFields(
            const Document &document, const std::span<const std::string_view> fields, const bool required,
            const std::string_view origin, const std::string &prefix, Diagnostics &diagnostics) {
            for (const auto name: fields) {
                const bool present = std::ranges::find(document.m_entries, name, &Entry::m_key) !=
                                     document.m_entries.end();
                if (present != required) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(document, name, origin), prefix + std::string{name},
                            required ? "Required record field is missing" : "Field is forbidden for this record",
                            required ? EConfigErrc::missing_value : EConfigErrc::unexpected_value));
                }
            }
            return {};
        }

        [[nodiscard]] Expected<FeatureOperation> compileFeatureOperation(
            const std::string &key, const Document &document, const FeatureBounds &bounds,
            const ContentCatalog &materials, const std::string_view origin, const std::string &prefix,
            Diagnostics &diagnostics) {
            constexpr std::string_view required[] = {"order", "action", "shape", "x_fine", "y_fine"};
            auto record = resolveRecord<FeatureOperationRecord>(document, required, origin, diagnostics, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
            const bool capsule = record->m_shape == EFeatureShape::capsule;
            constexpr std::string_view box_fields[] = {"width_fine", "height_fine"};
            constexpr std::string_view capsule_fields[] = {"end_x_fine", "end_y_fine", "radius_m"};
            constexpr std::string_view material_fields[] = {"material"};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent, conditionalFields(document, box_fields, not capsule, origin, prefix, diagnostics));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent, conditionalFields(document, capsule_fields, capsule, origin, prefix, diagnostics));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                conditionalFields(
                    document, material_fields, record->m_action == EFeatureAction::paint, origin, prefix, diagnostics));

            double min_x = record->m_x_fine, min_y = record->m_y_fine;
            double max_x{}, max_y{};
            if (capsule) {
                const double radius_fine = *record->m_radius_m * 3.0;
                min_x = (std::min) (record->m_x_fine, *record->m_end_x_fine) - radius_fine;
                min_y = (std::min) (record->m_y_fine, *record->m_end_y_fine) - radius_fine;
                max_x = (std::max) (record->m_x_fine, *record->m_end_x_fine) + radius_fine;
                max_y = (std::max) (record->m_y_fine, *record->m_end_y_fine) + radius_fine;
            } else {
                max_x = min_x + *record->m_width_fine;
                max_y = min_y + *record->m_height_fine;
            }
            if (min_x < bounds.m_min_x_fine or
                min_y < bounds.m_min_y_fine or
                max_x > bounds.m_max_x_fine or
                max_y > bounds.m_max_y_fine) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(document, "shape", origin), prefix + "shape",
                                       "Operation geometry exceeds its reservation"));
            }
            u16 material_id{};
            if (record->m_action == EFeatureAction::paint) {
                auto material = terrainReference(
                    document, "material", record->m_material, materials, origin, prefix, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, material);
                material_id = *material;
            }
            return FeatureOperation{
                key, record->m_order, record->m_action, record->m_shape, record->m_x_fine, record->m_y_fine,
                record->m_width_fine.value_or(0u), record->m_height_fine.value_or(0u), record->m_end_x_fine.value_or(0),
                record->m_end_y_fine.value_or(0), record->m_radius_m.value_or(0.0), material_id};
        }

        [[nodiscard]] Expected<FeatureDefinition> compileFeature(
            const std::string &key, const Document &document, const ContentCatalog &materials,
            const std::string_view origin, Diagnostics &diagnostics) {
            const std::string prefix = "features." + key + ".";
            auto operations = splitChildren(document, "operations", kMaxFeatureOperations, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, operations);
            auto markers = splitChildren(operations->m_base, "markers", kMaxFeatureMarkers, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, markers);
            constexpr std::string_view required[] = {
                "display_name", "allow_horizontal_reflection", "reservation_min_x_fine", "reservation_min_y_fine",
                "reservation_max_x_fine", "reservation_max_y_fine"};
            auto record = resolveRecord<FeatureRecord>(markers->m_base, required, origin, diagnostics, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
            const FeatureBounds bounds{
                record->m_min_x_fine, record->m_min_y_fine, record->m_max_x_fine, record->m_max_y_fine};
            if (bounds.m_min_x_fine >= bounds.m_max_x_fine or
                bounds.m_min_y_fine >= bounds.m_max_y_fine or
                bounds.m_max_x_fine - bounds.m_min_x_fine > 4096 or
                bounds.m_max_y_fine - bounds.m_min_y_fine > 4096) {
                const bool invalid_x = bounds.m_min_x_fine >= bounds.m_max_x_fine or
                                       bounds.m_max_x_fine - bounds.m_min_x_fine > 4096;
                const std::string property = invalid_x ? "reservation_max_x_fine" : "reservation_max_y_fine";
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(markers->m_base, property, origin), prefix + property,
                                       "Reservation must have positive extents no larger than the world"));
            }
            if (operations->m_children.empty()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, prefix + "operations",
                                       "At least one terrain operation is required", EConfigErrc::missing_value));
            }
            FeatureDefinition result{
                key, std::move(record->m_display_name), record->m_allow_horizontal_reflection, bounds, {}, {}};
            for (const auto &[operation_key, operation_document]: operations->m_children) {
                auto operation = compileFeatureOperation(
                    operation_key, operation_document, bounds, materials, origin,
                    prefix + "operations." + operation_key + ".", diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, operation);
                result.m_operations.push_back(std::move(*operation));
            }
            std::ranges::sort(result.m_operations, {}, &FeatureOperation::m_order);
            for (std::size_t i = 1u; i < result.m_operations.size(); ++i) {
                if (result.m_operations[i - 1u].m_order == result.m_operations[i].m_order) {
                    const auto &operation = result.m_operations[i];
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(operations->m_children.at(operation.m_key), "order", origin),
                            prefix + "operations." + operation.m_key + ".order", "Operation orders must be unique",
                            EConfigErrc::duplicate_key));
                }
            }

            constexpr std::string_view marker_required[] = {"role", "x_fine", "y_fine"};
            for (const auto &[marker_key, marker_document]: markers->m_children) {
                const std::string marker_prefix = prefix + "markers." + marker_key + ".";
                auto marker = resolveRecord<FeatureMarkerRecord>(
                    marker_document, marker_required, origin, diagnostics, marker_prefix);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, marker);
                if (marker->m_x_fine < bounds.m_min_x_fine or
                    marker->m_x_fine >= bounds.m_max_x_fine or
                    marker->m_y_fine < bounds.m_min_y_fine or
                    marker->m_y_fine >= bounds.m_max_y_fine) {
                    const bool invalid_x = marker->m_x_fine < bounds.m_min_x_fine or
                                           marker->m_x_fine >= bounds.m_max_x_fine;
                    const std::string property = invalid_x ? "x_fine" : "y_fine";
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, fieldLocation(marker_document, property, origin),
                                           marker_prefix + property, "Marker cell must lie inside its reservation"));
                }
                result.m_markers.push_back({marker_key, marker->m_role, marker->m_x_fine, marker->m_y_fine});
            }
            return result;
        }

        [[nodiscard]] std::error_code worldInterval(
            const Document &document, const std::string_view maximum_property, const double minimum,
            const double maximum, const bool allow_equal, const std::string_view origin, const std::string &prefix,
            Diagnostics &diagnostics) {
            if (minimum > maximum or (not allow_equal and minimum == maximum)) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(document, maximum_property, origin),
                                       prefix + std::string{maximum_property},
                                       "Interval endpoints must be ordered and have the required extent"));
            }
            return {};
        }

        [[nodiscard]] std::error_code worldBiomeReference(
            const Document &document, const std::string_view biome, const std::span<const WorldBiomeChoice> choices,
            const std::string_view origin, const std::string &prefix, Diagnostics &diagnostics) {
            if (std::ranges::none_of(choices, [&](const auto &choice) { return choice.m_biome == biome; })) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(document, "biome", origin), prefix + "biome",
                                       "Rule biome must be enabled by this profile"));
            }
            return {};
        }

        [[nodiscard]] Expected<const FeatureDefinition *> worldFeatureReference(
            const Document &document, const std::string_view property, const std::string_view key,
            const std::span<const FeatureDefinition> features, const std::string_view origin, const std::string &prefix,
            Diagnostics &diagnostics) {
            const auto found = std::ranges::find(features, key, &FeatureDefinition::m_key);
            if (found == features.end()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(document, property, origin), prefix + std::string{property},
                        "Unknown feature template reference: " + std::string{key}));
            }
            return std::addressof(*found);
        }

        [[nodiscard]] Expected<WorldRuleDefinition> compileWorldRule(
            const std::string &key, const Document &document, const ContentCatalog &materials,
            const std::span<const FieldDefinition> fields, const std::span<const WorldBiomeChoice> choices,
            const std::span<const FeatureDefinition> features, const std::string_view origin, const std::string &prefix,
            Diagnostics &diagnostics) {
            constexpr std::string_view header_required[] = {"kind", "phase", "order"};
            Document header_document{};
            for (const auto &entry: document.m_entries) {
                if (std::ranges::find(header_required, entry.m_key) != std::end(header_required)) {
                    header_document.m_entries.push_back(entry);
                }
            }
            auto header = resolveRecord<WorldRuleHeader>(header_document, header_required, origin, diagnostics, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, header);
            EWorldRulePhase expected_phase = EWorldRulePhase::carve;
            switch (header->m_kind) {
                case EWorldRuleKind::vein_worm: expected_phase = EWorldRulePhase::deposit; break;
                case EWorldRuleKind::feature: expected_phase = EWorldRulePhase::stamp; break;
                case EWorldRuleKind::matter_fill: expected_phase = EWorldRulePhase::fill; break;
                default: break;
            }
            if (header->m_phase != expected_phase) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(document, "phase", origin), prefix + "phase",
                                       "Rule kind is incompatible with this generation phase"));
            }
            WorldRuleDefinition result{key, header->m_order, header->m_kind, header->m_phase, {}};
            switch (header->m_kind) {
                case EWorldRuleKind::threshold_cave: {
                    constexpr std::string_view required[] = {
                        "biome", "field", "depth_min_normalized", "depth_max_normalized", "threshold_min",
                        "threshold_max"};
                    auto record = resolveRecord<WorldCaveRecord>(document, required, origin, diagnostics, prefix);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        worldBiomeReference(document, record->m_biome, choices, origin, prefix, diagnostics));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        requireFieldReference(document, "field", record->m_field, fields, origin, prefix, diagnostics));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, worldInterval(
                                           document, "depth_max_normalized", record->m_depth_min_normalized,
                                           record->m_depth_max_normalized, false, origin, prefix, diagnostics));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, worldInterval(
                                           document, "threshold_max", record->m_threshold_min, record->m_threshold_max,
                                           false, origin, prefix, diagnostics));
                    result.m_parameters = WorldCaveRule{
                        std::move(record->m_biome), std::move(record->m_field), record->m_depth_min_normalized,
                        record->m_depth_max_normalized, record->m_threshold_min, record->m_threshold_max};
                    break;
                }
                case EWorldRuleKind::carve_worm:
                case EWorldRuleKind::vein_worm: {
                    constexpr std::string_view required[] = {
                        "biome", "count", "depth_min_normalized", "depth_max_normalized", "length_min_m",
                        "length_max_m", "radius_min_m", "radius_max_m", "turn_probability"};
                    auto record = resolveRecord<WorldWormRecord>(document, required, origin, diagnostics, prefix);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        worldBiomeReference(document, record->m_biome, choices, origin, prefix, diagnostics));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, worldInterval(
                                           document, "depth_max_normalized", record->m_depth_min_normalized,
                                           record->m_depth_max_normalized, false, origin, prefix, diagnostics));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, worldInterval(
                                           document, "length_max_m", record->m_length_min_m, record->m_length_max_m,
                                           true, origin, prefix, diagnostics));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, worldInterval(
                                           document, "radius_max_m", record->m_radius_min_m, record->m_radius_max_m,
                                           true, origin, prefix, diagnostics));
                    if (record->m_turn_smoothing == 0.0 and (record->m_radius_variation != 0.0 or
                                                                record->m_chamber_probability != 0.0 or
                                                                record->m_chamber_scale != 1.0)) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyContent, reject(
                                               diagnostics, origin, prefix + "turn_smoothing",
                                               "Radius variation and chambers require positive turn smoothing"));
                    }
                    constexpr std::string_view material_fields[] = {"material", "host_material"};
                    const bool vein = header->m_kind == EWorldRuleKind::vein_worm;
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, conditionalFields(document, material_fields, vein, origin, prefix, diagnostics));
                    u16 material_id{}, host_id{};
                    if (vein) {
                        auto material = terrainReference(
                            document, "material", record->m_material, materials, origin, prefix, diagnostics);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, material);
                        auto host = terrainReference(
                            document, "host_material", record->m_host_material, materials, origin, prefix, diagnostics);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, host);
                        material_id = *material;
                        host_id = *host;
                    }
                    result.m_parameters = WorldWormRule{
                        std::move(record->m_biome), record->m_count, record->m_depth_min_normalized,
                        record->m_depth_max_normalized, record->m_length_min_m, record->m_length_max_m,
                        record->m_radius_min_m, record->m_radius_max_m, record->m_turn_probability, material_id,
                        host_id, record->m_turn_smoothing, record->m_radius_variation, record->m_chamber_probability,
                        record->m_chamber_scale};
                    break;
                }
                case EWorldRuleKind::connect_rooms: {
                    constexpr std::string_view required[] = {"radius_m", "extra_links"};
                    auto record = resolveRecord<WorldConnectionRecord>(document, required, origin, diagnostics, prefix);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
                    result.m_parameters = WorldConnectionRule{
                        record->m_radius_m, record->m_extra_links, record->m_natural_anchors, record->m_wells,
                        record->m_branches, record->m_roughness_m};
                    break;
                }
                case EWorldRuleKind::feature: {
                    constexpr std::string_view required[] = {
                        "feature", "biome", "depth_min_normalized", "depth_max_normalized", "spacing_m", "min_count",
                        "max_count", "attempts", "priority", "reflect"};
                    auto record = resolveRecord<WorldFeatureRecord>(document, required, origin, diagnostics, prefix);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        worldBiomeReference(document, record->m_biome, choices, origin, prefix, diagnostics));
                    auto feature = worldFeatureReference(
                        document, "feature", record->m_feature, features, origin, prefix, diagnostics);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, feature);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, worldInterval(
                                           document, "depth_max_normalized", record->m_depth_min_normalized,
                                           record->m_depth_max_normalized, false, origin, prefix, diagnostics));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, worldInterval(
                                           document, "max_count", record->m_min_count, record->m_max_count, true,
                                           origin, prefix, diagnostics));
                    if (record->m_reflect and not (*feature)->m_allow_horizontal_reflection) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyContent, reject(
                                               diagnostics, fieldLocation(document, "reflect", origin),
                                               prefix + "reflect", "Template does not allow horizontal reflection"));
                    }
                    result.m_parameters = WorldFeatureRule{
                        std::move(record->m_feature), std::move(record->m_biome), record->m_depth_min_normalized,
                        record->m_depth_max_normalized, record->m_spacing_m, record->m_min_count, record->m_max_count,
                        record->m_attempts, record->m_priority, record->m_reflect};
                    break;
                }
                case EWorldRuleKind::matter_fill: {
                    constexpr std::string_view required[] = {
                        "feature_rule", "marker", "material", "temperature_k", "bulk_density_kg_m3"};
                    auto record = resolveRecord<WorldMatterRecord>(document, required, origin, diagnostics, prefix);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
                    const auto *const material = materials.findMaterial(record->m_material);
                    if (not material or material->m_representation != EMaterialRepresentation::particle) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyContent,
                            reject(
                                diagnostics, fieldLocation(document, "material", origin), prefix + "material",
                                "Initial matter requires a particle material reference"));
                    }
                    constexpr std::string_view height_fields[] = {"fill_height_m"};
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, conditionalFields(
                                           document, height_fields, material->m_phase != EMaterialPhase::gas, origin,
                                           prefix, diagnostics));
                    if (record->m_fill_height_m and not discreteFineCellLength(std::abs(*record->m_fill_height_m))) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyContent,
                            reject(
                                diagnostics, fieldLocation(document, "fill_height_m", origin), prefix + "fill_height_m",
                                "Fill height must be an exact integer fine-cell boundary in metres"));
                    }
                    result.m_parameters = WorldMatterRule{
                        std::move(record->m_feature_rule), std::move(record->m_marker), material->m_numeric_id,
                        record->m_temperature_k, record->m_bulk_density_kg_m3, record->m_fill_height_m};
                    break;
                }
            }
            return result;
        }

        [[nodiscard]] std::error_code compileWorldBiomes(
            const std::map<std::string, Document, std::less<>> &documents, const Document &base,
            const std::span<const BiomeDefinition> biomes, WorldProfileDefinition &result,
            const std::string_view origin, const std::string &prefix, Diagnostics &diagnostics) {
            constexpr std::string_view choice_required[] = {
                "biome", "weight", "required_count", "depth_min_normalized", "depth_max_normalized", "distance_min_m",
                "distance_max_m"};
            double total_weight = 0.0;
            u32 required_sites = 0u;
            for (const auto &[choice_key, choice_document]: documents) {
                const std::string choice_prefix = prefix + "biomes." + choice_key + ".";
                auto choice = resolveRecord<WorldBiomeRecord>(
                    choice_document, choice_required, origin, diagnostics, choice_prefix);
                PPR_RETURN_ERROR_ON_FAIL(ColonyContent, choice);
                if (std::ranges::find(biomes, choice->m_biome, &BiomeDefinition::m_key) == biomes.end()) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, fieldLocation(choice_document, "biome", origin),
                                           choice_prefix + "biome", "Unknown biome reference"));
                }
                PPR_RETURN_ERROR_ON_FAIL(
                    ColonyContent, worldInterval(
                                       choice_document, "depth_max_normalized", choice->m_depth_min_normalized,
                                       choice->m_depth_max_normalized, false, origin, choice_prefix, diagnostics));
                PPR_RETURN_ERROR_ON_FAIL(
                    ColonyContent, worldInterval(
                                       choice_document, "distance_max_m", choice->m_distance_min_m,
                                       choice->m_distance_max_m, true, origin, choice_prefix, diagnostics));
                required_sites += choice->m_required_count;
                if (required_sites > result.m_max_sites) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(choice_document, "required_count", origin),
                            choice_prefix + "required_count", "Required biome counts exceed the profile site cap"));
                }
                const double next_weight = total_weight + choice->m_weight;
                if (not std::isfinite(next_weight) or next_weight == total_weight) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, fieldLocation(choice_document, "weight", origin),
                                           choice_prefix + "weight", "Cumulative biome weight is not representable"));
                }
                total_weight = next_weight;
                result.m_biomes.push_back(
                    {choice_key, std::move(choice->m_biome), choice->m_weight, choice->m_required_count,
                        choice->m_depth_min_normalized, choice->m_depth_max_normalized, choice->m_distance_min_m,
                        choice->m_distance_max_m});
            }
            double cumulative = 0.0, previous = 0.0;
            for (const auto &choice: result.m_biomes) {
                cumulative += choice.m_weight;
                const double fraction = cumulative / total_weight;
                if (fraction <= previous) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, fieldLocation(documents.at(choice.m_key), "weight", origin),
                                           prefix + "biomes." + choice.m_key + ".weight",
                                           "Normalized biome weight interval is not representable"));
                }
                previous = fraction;
            }
            if (std::ranges::none_of(
                    result.m_biomes, [&](const auto &choice) { return choice.m_biome == result.m_start_biome; })) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(base, "start_biome", origin), prefix + "start_biome",
                                       "Starting biome must be enabled by this profile"));
            }

            return {};
        }

        [[nodiscard]] std::error_code compileStartResources(
            const std::map<std::string, Document, std::less<>> &documents, const ContentCatalog &materials,
            const FeatureDefinition &start, const FeatureMarker &spawn, WorldProfileDefinition &result,
            const std::string_view origin, const std::string &prefix, Diagnostics &diagnostics) {
            constexpr std::string_view resource_required[] = {
                "marker", "material", "minimum_fine_cells", "max_distance_m"};
            for (const auto &[resource_key, resource_document]: documents) {
                const std::string resource_prefix = prefix + "start_resources." + resource_key + ".";
                auto resource = resolveRecord<StartResourceRecord>(
                    resource_document, resource_required, origin, diagnostics, resource_prefix);
                PPR_RETURN_ERROR_ON_FAIL(ColonyContent, resource);
                auto id = terrainReference(
                    resource_document, "material", resource->m_material, materials, origin, resource_prefix,
                    diagnostics);
                PPR_RETURN_ERROR_ON_FAIL(ColonyContent, id);
                if (not *materials.findMaterial(*id)->m_diggable) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(resource_document, "material", origin),
                            resource_prefix + "material", "Guaranteed starting resource must be diggable"));
                }
                const auto marker = std::ranges::find(start.m_markers, resource->m_marker, &FeatureMarker::m_key);
                if (marker == start.m_markers.end() or
                    marker->m_role != EFeatureMarkerRole::resource_anchor or
                    std::ranges::any_of(result.m_start_resources, [&](const auto &existing) {
                        return existing.m_marker == resource->m_marker;
                    })) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(resource_document, "marker", origin), resource_prefix + "marker",
                            "Resource alcoves require distinct resource-anchor markers in the start template"));
                }
                const double dx = static_cast<double>(marker->m_x_fine - spawn.m_x_fine) / 3.0;
                const double dy = static_cast<double>(marker->m_y_fine - spawn.m_y_fine) / 3.0;
                if (std::hypot(dx, dy) > resource->m_max_distance_m) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, fieldLocation(resource_document, "max_distance_m", origin),
                                           resource_prefix + "max_distance_m",
                                           "Resource anchor lies beyond its maximum start distance"));
                }
                result.m_start_resources.push_back(
                    {resource_key, std::move(resource->m_marker), *id, resource->m_minimum_fine_cells,
                        resource->m_max_distance_m});
            }

            return {};
        }

        [[nodiscard]] std::error_code compileWorldRules(
            const std::map<std::string, Document, std::less<>> &documents, const ContentCatalog &materials,
            const std::span<const FieldDefinition> fields, const std::span<const FeatureDefinition> features,
            WorldProfileDefinition &result, const std::string_view origin, const std::string &prefix,
            Diagnostics &diagnostics) {
            u32 worm_count = 0u, feature_count = 0u, connection_count = 0u;
            u64 origin_attempts = 0u;
            for (const auto &[rule_key, rule_document]: documents) {
                const std::string rule_prefix = prefix + "rules." + rule_key + ".";
                auto rule = compileWorldRule(
                    rule_key, rule_document, materials, fields, result.m_biomes, features, origin, rule_prefix,
                    diagnostics);
                PPR_RETURN_ERROR_ON_FAIL(ColonyContent, rule);
                if (const auto *const worm = std::get_if<WorldWormRule>(&rule->m_parameters)) {
                    worm_count += worm->m_count;
                    origin_attempts += static_cast<u64>(worm->m_count) * sim::kGenerationWormOriginAttempts;
                }
                if (const auto *const feature = std::get_if<WorldFeatureRule>(&rule->m_parameters)) {
                    feature_count += feature->m_max_count;
                    origin_attempts += static_cast<u64>(feature->m_max_count) * feature->m_attempts;
                }
                if (rule->m_kind == EWorldRuleKind::connect_rooms) {
                    ++connection_count;
                }
                if (worm_count > 4096u or
                    feature_count > 1024u or
                    connection_count > 1u or
                    origin_attempts > sim::kMaxGenerationOriginAttempts) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(rule_document, "kind", origin), rule_prefix + "kind",
                            "Profile exceeds aggregate limits: 4096 worms, 1024 feature instances, one room-connection rule, 4194304 origin attempts",
                            EConfigErrc::out_of_range));
                }
                result.m_rules.push_back(std::move(*rule));
            }
            std::ranges::sort(result.m_rules, [](const auto &a, const auto &b) {
                return std::tie(a.m_phase, a.m_order, a.m_key) < std::tie(b.m_phase, b.m_order, b.m_key);
            });
            for (std::size_t i = 1u; i < result.m_rules.size(); ++i) {
                const auto &rule = result.m_rules[i];
                const auto &previous_rule = result.m_rules[i - 1u];
                if (rule.m_phase == previous_rule.m_phase and rule.m_order == previous_rule.m_order) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(documents.at(rule.m_key), "order", origin),
                            prefix + "rules." + rule.m_key + ".order", "Rule orders must be unique within their phase",
                            EConfigErrc::duplicate_key));
                }
            }
            for (const auto &rule: result.m_rules) {
                const auto *const fill = std::get_if<WorldMatterRule>(&rule.m_parameters);
                if (not fill) {
                    continue;
                }
                const auto &rule_document = documents.at(rule.m_key);
                const std::string rule_prefix = prefix + "rules." + rule.m_key + ".";
                const auto placement = std::ranges::find(
                    result.m_rules, fill->m_feature_rule, &WorldRuleDefinition::m_key);
                if (placement == result.m_rules.end() or placement->m_kind != EWorldRuleKind::feature) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, fieldLocation(rule_document, "feature_rule", origin),
                                           rule_prefix + "feature_rule",
                                           "Fill must reference a feature-placement rule in this profile"));
                }
                const auto &feature_key = std::get<WorldFeatureRule>(placement->m_parameters).m_feature;
                const auto feature = std::ranges::find(features, feature_key, &FeatureDefinition::m_key);
                const auto marker = std::ranges::find(feature->m_markers, fill->m_marker, &FeatureMarker::m_key);
                if (marker == feature->m_markers.end() or marker->m_role != EFeatureMarkerRole::sealed_pocket) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(rule_document, "marker", origin), rule_prefix + "marker",
                            "Fill requires a sealed-pocket marker in its placed feature"));
                }
                if (fill->m_fill_height_m and
                    (*fill->m_fill_height_m <= static_cast<double>(feature->m_reservation.m_min_y_fine) / 3.0 or
                        *fill->m_fill_height_m > static_cast<double>(feature->m_reservation.m_max_y_fine) / 3.0)) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(rule_document, "fill_height_m", origin),
                            rule_prefix + "fill_height_m",
                            "Local fill height must lie above the reservation bottom and at or below its top"));
                }
            }
            return {};
        }

        [[nodiscard]] Expected<WorldProfileDefinition> compileWorldProfile(
            const std::string &key, const Document &document, const ContentCatalog &materials,
            const std::span<const FieldDefinition> fields, const std::span<const BiomeDefinition> biomes,
            const std::span<const FeatureDefinition> features, const std::string_view origin,
            Diagnostics &diagnostics) {
            const std::string prefix = "worlds." + key + ".";
            auto choices = splitChildren(document, "biomes", kMaxContentBiomes, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, choices);
            auto rules = splitChildren(choices->m_base, "rules", kMaxWorldRules, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, rules);
            auto resources = splitChildren(rules->m_base, "start_resources", kMaxStartResources, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resources);
            constexpr std::string_view required[] = {
                "display_name", "default_wall_material", "boundary_fine", "min_sites", "max_sites", "site_attempts",
                "site_spacing_m", "boundary_warp_amplitude_m", "boundary_warp_x_field", "boundary_warp_y_field",
                "start_biome", "start_feature", "start_marker", "start_x_fine", "start_y_fine", "start_clearance_m",
                "start_route_width_m", "background_stack"};
            auto record = resolveRecord<WorldProfileRecord>(resources->m_base, required, origin, diagnostics, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
            const auto &base = resources->m_base;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                worldInterval(
                    base, "max_sites", record->m_min_sites, record->m_max_sites, true, origin, prefix, diagnostics));
            auto wall = terrainReference(
                base, "default_wall_material", record->m_default_wall_material, materials, origin, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, wall);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent, requireFieldReference(
                                   base, "boundary_warp_x_field", record->m_boundary_warp_x_field, fields, origin,
                                   prefix, diagnostics));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent, requireFieldReference(
                                   base, "boundary_warp_y_field", record->m_boundary_warp_y_field, fields, origin,
                                   prefix, diagnostics));
            auto start = worldFeatureReference(
                base, "start_feature", record->m_start_feature, features, origin, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, start);
            const auto spawn = std::ranges::find((*start)->m_markers, record->m_start_marker, &FeatureMarker::m_key);
            if (spawn == (*start)->m_markers.end() or spawn->m_role != EFeatureMarkerRole::start) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(base, "start_marker", origin), prefix + "start_marker",
                        "Start marker must reference a start-role marker in the start template"));
            }
            const auto &reservation = (*start)->m_reservation;
            const i32 start_x = static_cast<i32>(record->m_start_x_fine);
            const i32 start_y = static_cast<i32>(record->m_start_y_fine);
            const i32 boundary = static_cast<i32>(record->m_boundary_fine);
            const bool invalid_x = start_x + reservation.m_min_x_fine < boundary or
                                   start_x + reservation.m_max_x_fine > 4096 - boundary;
            const bool invalid_y = start_y + reservation.m_min_y_fine < boundary or
                                   start_y + reservation.m_max_y_fine > 4096 - boundary;
            if (invalid_x or invalid_y) {
                const std::string property = invalid_x ? "start_x_fine" : "start_y_fine";
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(base, property, origin), prefix + property,
                                       "Start reservation must fit inside the permanent world boundary"));
            }
            if (record->m_start_clearance_m >
                static_cast<double>(reservation.m_max_y_fine - reservation.m_min_y_fine) / 3.0) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(base, "start_clearance_m", origin),
                                       prefix + "start_clearance_m", "Clearance exceeds the start reservation height"));
            }
            if (record->m_start_route_width_m >
                static_cast<double>(reservation.m_max_x_fine - reservation.m_min_x_fine) / 3.0) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(base, "start_route_width_m", origin), prefix + "start_route_width_m",
                        "Route width exceeds the start reservation width"));
            }
            if (choices->m_children.empty() or resources->m_children.size() < 2u or rules->m_children.empty()) {
                const std::string property = choices->m_children.empty()
                                                 ? "biomes"
                                                 : (resources->m_children.size() < 2u ? "start_resources" : "rules");
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, origin, prefix + property,
                        "Profile requires biome choices, at least two resource alcoves and generation rules",
                        EConfigErrc::missing_value));
            }

            WorldProfileDefinition result{};
            result.m_key = key;
            result.m_display_name = std::move(record->m_display_name);
            result.m_default_wall_material_id = *wall;
            result.m_boundary_fine = record->m_boundary_fine;
            result.m_min_sites = record->m_min_sites;
            result.m_max_sites = record->m_max_sites;
            result.m_site_attempts = record->m_site_attempts;
            result.m_site_spacing_m = record->m_site_spacing_m;
            result.m_boundary_warp_amplitude_m = record->m_boundary_warp_amplitude_m;
            result.m_boundary_warp_x_field = std::move(record->m_boundary_warp_x_field);
            result.m_boundary_warp_y_field = std::move(record->m_boundary_warp_y_field);
            result.m_start_biome = std::move(record->m_start_biome);
            result.m_start_feature = std::move(record->m_start_feature);
            result.m_start_marker = std::move(record->m_start_marker);
            result.m_start_x_fine = record->m_start_x_fine;
            result.m_start_y_fine = record->m_start_y_fine;
            result.m_start_clearance_m = record->m_start_clearance_m;
            result.m_start_route_width_m = record->m_start_route_width_m;
            result.m_background_stack = std::move(record->m_background_stack);
            if (record->m_surface_field) {
                for (const auto *name: {&record->m_surface_field, &record->m_bedrock_field}) {
                    if (not *name) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyContent, reject(
                                               diagnostics, origin, prefix + "surface_field",
                                               "Open surface requires surface and bedrock fields"));
                    }
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        requireFieldReference(base, "surface_field", **name, fields, origin, prefix, diagnostics));
                }
                if (not record->m_bedrock_material or
                    record->m_bedrock_thickness_m - record->m_bedrock_variation_m < 15.0 or
                    record->m_bedrock_thickness_m + record->m_bedrock_variation_m > 40.0) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, origin, prefix + "bedrock_thickness_m",
                                           "Bedrock must remain continuously 15 to 40 metres thick"));
                }
                auto bedrock = terrainReference(
                    base, "bedrock_material", *record->m_bedrock_material, materials, origin, prefix, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, bedrock);
                if (materials.findMaterial(*bedrock)->m_diggable.value_or(true)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(diagnostics, origin, prefix + "bedrock_material", "Bedrock must be undiggable terrain"));
                }
                result.m_bedrock_material_id = *bedrock;
            } else if (record->m_bedrock_field or record->m_bedrock_material or record->m_start_depth_m) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, prefix + "surface_field",
                                       "Surface-relative controls require a surface field"));
            }
            result.m_surface_field = std::move(record->m_surface_field);
            result.m_bedrock_field = std::move(record->m_bedrock_field);
            result.m_surface_elevation_m = record->m_surface_elevation_m;
            result.m_surface_amplitude_m = record->m_surface_amplitude_m;
            result.m_bedrock_thickness_m = record->m_bedrock_thickness_m;
            result.m_bedrock_variation_m = record->m_bedrock_variation_m;
            result.m_start_depth_m = record->m_start_depth_m;

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                compileWorldBiomes(choices->m_children, base, biomes, result, origin, prefix, diagnostics));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                compileStartResources(
                    resources->m_children, materials, **start, *spawn, result, origin, prefix, diagnostics));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                compileWorldRules(rules->m_children, materials, fields, features, result, origin, prefix, diagnostics));
            return result;
        }

        [[nodiscard]] Expected<BiomeDefinition> compileBiome(
            const std::string &key, const Document &document, const ContentCatalog &materials,
            const std::span<const FieldDefinition> fields, const std::string_view origin, Diagnostics &diagnostics) {
            const std::string prefix = "biomes." + key + ".";
            auto composition = splitChildren(document, "composition", kMaxBiomeCompositionBands, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, composition);
            auto deposits = splitChildren(composition->m_base, "deposits", kMaxBiomeDeposits, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, deposits);
            constexpr std::string_view required[] = {
                "display_name", "preview_color_rgba", "temperature_base_k", "temperature_variation_k",
                "temperature_field"};
            auto record = resolveRecord<BiomeRecord>(deposits->m_base, required, origin, diagnostics, prefix);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, record);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent, requireFieldReference(
                                   deposits->m_base, "temperature_field", record->m_temperature_field, fields, origin,
                                   prefix, diagnostics));
            u32 color{};
            const auto [end, error] = std::from_chars(
                record->m_preview_color_rgba.data(),
                record->m_preview_color_rgba.data() + record->m_preview_color_rgba.size(), color, 16);
            if (record->m_preview_color_rgba.size() != 8u or
                error != std::errc{} or
                end != record->m_preview_color_rgba.data() + 8u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(deposits->m_base, "preview_color_rgba", origin),
                        prefix + "preview_color_rgba", "Expected exactly eight hexadecimal RRGGBBAA digits"));
            }
            if (record->m_temperature_base_k <= record->m_temperature_variation_k) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(deposits->m_base, "temperature_variation_k", origin),
                                       prefix + "temperature_variation_k",
                                       "Temperature variation must remain below the positive base temperature"));
            }
            BiomeDefinition result{
                key, std::move(record->m_display_name), color, record->m_temperature_base_k,
                record->m_temperature_variation_k, std::move(record->m_temperature_field), {}, {}};
            result.m_relaxation_passes = record->m_relaxation_passes;
            result.m_relaxation_adoption = record->m_relaxation_adoption;
            result.m_sprinkle_probability = record->m_sprinkle_probability;
            if (record->m_pore_field) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    requireFieldReference(
                        deposits->m_base, "pore_field", *record->m_pore_field, fields, origin, prefix, diagnostics));
                result.m_pore_field = std::move(record->m_pore_field);
            }
            result.m_pore_probability = record->m_pore_probability;
            result.m_pore_radius_min_m = record->m_pore_radius_min_m;
            result.m_pore_radius_max_m = record->m_pore_radius_max_m;
            result.m_pore_affinity_min = record->m_pore_affinity_min;
            result.m_pore_passes = record->m_pore_passes;
            result.m_finish_passes = record->m_finish_passes;
            result.m_erosion_passes = record->m_erosion_passes;
            result.m_finish_adoption = record->m_finish_adoption;
            if (record->m_soil_material) {
                auto id = terrainReference(
                    deposits->m_base, "soil_material", *record->m_soil_material, materials, origin, prefix,
                    diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, id);
                result.m_soil_material_id = *id;
            }
            if (record->m_weathered_material) {
                auto id = terrainReference(
                    deposits->m_base, "weathered_material", *record->m_weathered_material, materials, origin, prefix,
                    diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, id);
                result.m_weathered_material_id = *id;
            }
            result.m_soil_depth_m = record->m_soil_depth_m;
            result.m_weathered_depth_m = record->m_weathered_depth_m;
            if (record->m_formation_material) {
                auto id = terrainReference(
                    deposits->m_base, "formation_material", *record->m_formation_material, materials, origin, prefix,
                    diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, id);
                result.m_formation_material_id = *id;
            }
            if (record->m_rind_material) {
                auto id = terrainReference(
                    deposits->m_base, "rind_material", *record->m_rind_material, materials, origin, prefix,
                    diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, id);
                result.m_rind_material_id = *id;
            }
            result.m_formation_probability = record->m_formation_probability;
            result.m_formation_length_m = record->m_formation_length_m;
            if (result.m_relaxation_passes + result.m_pore_passes + result.m_finish_passes + result.m_erosion_passes >
                100u) {
                const auto pass_budget_error = config::make_error_code(EConfigErrc::cross_field_invalid);
                for (const std::string_view property:
                    {"relaxation_passes", "pore_passes", "finish_passes", "erosion_passes"}) {
                    diagnostics.add(
                        std::string{fieldLocation(deposits->m_base, property, origin)}, prefix + std::string{property},
                        "Relaxation, pore, finishing and erosion passes must total at most 100", pass_budget_error);
                }
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, pass_budget_error);
            }
            if (result.m_pore_radius_min_m > result.m_pore_radius_max_m) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(deposits->m_base, "pore_radius_min_m", origin),
                        prefix + "pore_radius_min_m", "Minimum pore radius must not exceed maximum pore radius"));
            }
            if ((result.m_pore_probability > 0.0 or result.m_pore_passes > 0u) and not result.m_pore_field) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(deposits->m_base, "pore_field", origin),
                                       prefix + "pore_field", "Enabled pores require a pore field"));
            }
            if (result.m_soil_depth_m > 0.0 and result.m_soil_material_id == 0u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, fieldLocation(deposits->m_base, "soil_material", origin),
                                       prefix + "soil_material", "Positive soil depth requires a soil material"));
            }
            if (result.m_weathered_depth_m > 0.0 and result.m_weathered_material_id == 0u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(deposits->m_base, "weathered_material", origin),
                        prefix + "weathered_material", "Positive weathered depth requires a weathered material"));
            }
            if (result.m_formation_probability > 0.0 and result.m_formation_material_id == 0u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, fieldLocation(deposits->m_base, "formation_material", origin),
                        prefix + "formation_material", "Enabled formations require a formation material"));
            }

            if (record->m_lining_material.has_value() != (record->m_lining_thickness_fine != 0u)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, prefix + "lining_material",
                                       "Lining material and positive thickness must be specified together"));
            }
            if (record->m_lining_material) {
                auto lining = terrainReference(
                    deposits->m_base, "lining_material", *record->m_lining_material, materials, origin, prefix,
                    diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, lining);
                result.m_lining_material_id = *lining;
                result.m_lining_thickness_fine = record->m_lining_thickness_fine;
            }
            for (const auto &[band_key, band]: composition->m_children) {
                auto compiled = compileComposition(
                    band_key, band, materials, fields, origin, prefix + "composition." + band_key + ".", diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, compiled);
                result.m_composition.push_back(std::move(*compiled));
            }
            std::ranges::sort(result.m_composition, {}, &BiomeCompositionBand::m_order);
            double next_depth = 0.0;
            std::optional<u32> previous_order{};
            for (const auto &band: result.m_composition) {
                if (previous_order == band.m_order or band.m_depth_min_normalized != next_depth) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(composition->m_children.at(band.m_key), "order", origin),
                            prefix + "composition." + band.m_key + ".order",
                            "Band orders must be unique and depth bands contiguous in order"));
                }
                previous_order = band.m_order;
                next_depth = band.m_depth_max_normalized;
            }
            if (next_depth != 1.0) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, prefix + "composition",
                                       "Composition bands must cover all normalized depths from zero to one"));
            }
            for (const auto &[deposit_key, deposit]: deposits->m_children) {
                auto compiled = compileDeposit(
                    deposit_key, deposit, materials, fields, origin, prefix + "deposits." + deposit_key + ".",
                    diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, compiled);
                result.m_deposits.push_back(std::move(*compiled));
            }
            std::ranges::sort(result.m_deposits, {}, &BiomeThresholdDeposit::m_order);
            previous_order.reset();
            for (const auto &deposit: result.m_deposits) {
                if (previous_order == deposit.m_order) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, fieldLocation(deposits->m_children.at(deposit.m_key), "order", origin),
                            prefix + "deposits." + deposit.m_key + ".order", "Deposit order is duplicated",
                            EConfigErrc::duplicate_key));
                }
                previous_order = deposit.m_order;
            }
            return result;
        }
    }

    std::optional<ContentPropertyDescriptor> contentPropertyDescriptor(
        const EContentDocument category, const std::string_view relative_property, const config::Document &document,
        const std::string_view definition_key) {
        const auto nested = nestedProperty(relative_property);
        switch (category) {
            case EContentDocument::materials: return recordPropertyDescriptor<MaterialRecord>(relative_property);
            case EContentDocument::fields: return recordPropertyDescriptor<FieldRecord>(relative_property);
            case EContentDocument::biomes:
                if (not nested) {
                    return recordPropertyDescriptor<BiomeRecord>(relative_property);
                }
                if (nested->m_collection == "composition") {
                    if (const auto weighted = nestedProperty(nested->m_property);
                        weighted and weighted->m_collection == "materials") {
                        return recordPropertyDescriptor<WeightedMaterialRecord>(weighted->m_property);
                    }
                    return recordPropertyDescriptor<CompositionRecord>(nested->m_property);
                }
                if (nested->m_collection == "deposits") {
                    return recordPropertyDescriptor<DepositRecord>(nested->m_property);
                }
                break;
            case EContentDocument::features:
                if (not nested) {
                    return recordPropertyDescriptor<FeatureRecord>(relative_property);
                }
                if (nested->m_collection == "operations") {
                    return recordPropertyDescriptor<FeatureOperationRecord>(nested->m_property);
                }
                if (nested->m_collection == "markers") {
                    return recordPropertyDescriptor<FeatureMarkerRecord>(nested->m_property);
                }
                break;
            case EContentDocument::backgrounds:
                if (nested and nested->m_collection == "layers") {
                    return recordPropertyDescriptor<BackgroundLayerRecord>(nested->m_property);
                }
                break;
            case EContentDocument::worlds:
                if (not nested) {
                    return recordPropertyDescriptor<WorldProfileRecord>(relative_property);
                }
                if (nested->m_collection == "biomes") {
                    return recordPropertyDescriptor<WorldBiomeRecord>(nested->m_property);
                }
                if (nested->m_collection == "start_resources") {
                    return recordPropertyDescriptor<StartResourceRecord>(nested->m_property);
                }
                if (nested->m_collection == "rules") {
                    const std::string prefix =
                        "worlds." + std::string{definition_key} + ".rules." + std::string{nested->m_key} + ".";
                    const auto kind = documentRuleKind(document, prefix);
                    if (not kind) {
                        return recordPropertyDescriptor<WorldRuleHeader>(nested->m_property);
                    }
                    switch (*kind) {
                        case EWorldRuleKind::threshold_cave:
                            return recordPropertyDescriptor<WorldCaveRecord>(nested->m_property);
                        case EWorldRuleKind::carve_worm:
                        case EWorldRuleKind::vein_worm:
                            return recordPropertyDescriptor<WorldWormRecord>(nested->m_property);
                        case EWorldRuleKind::connect_rooms:
                            return recordPropertyDescriptor<WorldConnectionRecord>(nested->m_property);
                        case EWorldRuleKind::feature:
                            return recordPropertyDescriptor<WorldFeatureRecord>(nested->m_property);
                        case EWorldRuleKind::matter_fill:
                            return recordPropertyDescriptor<WorldMatterRecord>(nested->m_property);
                    }
                }
                break;
            case EContentDocument::count: break;
        }
        return std::nullopt;
    }
    Array<std::string> contentPropertyReferenceChoices(
        const EContentDocument category, const std::string_view relative_property,
        const std::array<config::Document, kContentDocumentCount> &documents, const std::string_view definition_key) {
        const auto index = static_cast<std::size_t>(category);
        if (index >= documents.size()) {
            return {};
        }
        const auto &document = documents[index];
        const auto descriptor = contentPropertyDescriptor(category, relative_property, document, definition_key);
        if (not descriptor or not descriptor->m_reference_category) {
            return {};
        }
        const auto nested = nestedProperty(relative_property);
        const std::string root = std::string{kContentCollections[index]} + "." + std::string{definition_key} + ".";
        if (category == EContentDocument::worlds) {
            if (nested and nested->m_collection == "rules" and nested->m_property == "biome") {
                Array<std::string> result;
                for (const auto &key: recordKeys(document, root + "biomes.")) {
                    const auto value = documentText(document, root + "biomes." + key + ".biome");
                    if (not value.empty()) {
                        result.emplace_back(value);
                    }
                }
                std::ranges::sort(result);
                result.erase(std::unique(result.begin(), result.end()), result.end());
                return result;
            }
            if (nested and nested->m_collection == "rules" and nested->m_property == "feature_rule") {
                auto result = recordKeys(document, root + "rules.");
                std::erase_if(result, [&](const auto &key) {
                    return documentRuleKind(document, root + "rules." + key + ".") != EWorldRuleKind::feature;
                });
                return result;
            }
            if (relative_property == "start_marker" or (nested and nested->m_property == "marker")) {
                std::string_view feature;
                if (nested and nested->m_collection == "rules") {
                    const auto placement = documentText(
                        document, root + "rules." + std::string{nested->m_key} + ".feature_rule");
                    feature = documentText(document, root + "rules." + std::string{placement} + ".feature");
                } else {
                    feature = documentText(document, root + "start_feature");
                }
                if (feature.empty()) {
                    return {};
                }
                return recordKeys(
                    documents[static_cast<std::size_t>(EContentDocument::features)],
                    "features." + std::string{feature} + ".markers.");
            }
        }
        const auto reference = static_cast<std::size_t>(*descriptor->m_reference_category);
        return recordKeys(documents[reference], std::string{kContentCollections[reference]} + ".");
    }

    std::span<const MaterialDefinition> ContentCatalog::materials() const noexcept {
        return {m_materials.data(), m_materials.size()};
    }

    const MaterialDefinition *ContentCatalog::findMaterial(const u16 numeric_id) const noexcept {
        const auto found = std::ranges::find(m_materials, numeric_id, &MaterialDefinition::m_numeric_id);
        return found == m_materials.end() ? nullptr : std::addressof(*found);
    }

    const MaterialDefinition *ContentCatalog::findMaterial(const std::string_view key) const noexcept {
        const auto found = std::ranges::lower_bound(m_materials, key, {}, &MaterialDefinition::m_key);
        return found == m_materials.end() or found->m_key != key ? nullptr : std::addressof(*found);
    }

    Expected<Array<BackgroundStackDefinition>> decodeBackgroundDefinitions(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) try {
        auto groups = groupRecords(bytes, origin, "backgrounds", kMaxContentWorlds, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, groups);
        Array<BackgroundStackDefinition> result{};
        for (const auto &[key, document]: *groups) {
            const std::string prefix = "backgrounds." + key + ".";
            auto layers = splitChildren(document, "layers", 4u, prefix, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, layers);
            if (not layers->m_base.m_entries.empty() or layers->m_children.size() != 4u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, origin, prefix + "layers",
                                       "A background stack requires exactly four layers and no other fields"));
            }
            BackgroundStackDefinition stack{.m_key = key};
            for (const auto &[layer_key, layer_document]: layers->m_children) {
                constexpr std::string_view required[] = {
                    "order", "texture", "color_rgba", "repeat_period_m", "parallax_factor"};
                auto layer = resolveRecord<BackgroundLayerRecord>(
                    layer_document, required, origin, diagnostics, prefix + "layers." + layer_key + ".");
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, layer);
                u32 color{};
                const auto [end, error] = std::from_chars(
                    layer->m_color_rgba.data(), layer->m_color_rgba.data() + layer->m_color_rgba.size(), color, 16);
                if (layer->m_color_rgba.size() != 8u or
                    error != std::errc{} or
                    end != layer->m_color_rgba.data() + 8u or
                    not validTexturePath(layer->m_texture)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, origin, prefix + "layers." + layer_key,
                                           "Expected a portable texture path and eight hexadecimal RRGGBBAA digits"));
                }
                stack.m_layers.push_back(
                    {layer_key, std::move(layer->m_texture),
                        std::string{fieldLocation(layer_document, "texture", origin)}, layer->m_order, color,
                        layer->m_repeat_period_m, layer->m_parallax_factor});
            }
            std::ranges::sort(stack.m_layers, {}, &BackgroundLayerDefinition::m_order);
            for (u32 order = 0u; order < 4u; ++order) {
                if (stack.m_layers[order].m_order != order or
                    (order == 0u and (stack.m_layers[order].m_color_rgba & 255u) != 255u)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, origin, prefix + "layers",
                            "Layer orders must be unique zero through three; the farthest layer tint must be opaque"));
                }
            }
            result.push_back(std::move(stack));
        }
        return result;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }

    Expected<ContentPackManifest> decodePackManifest(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) {
        if (bytes.size() > kMaxPackManifestBytes) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                reject(diagnostics, origin, "", "Pack manifest exceeds 64 KiB", EConfigErrc::out_of_range));
        }
        auto document = decode(bytes, origin, diagnostics, EDecodeMode::records);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
        constexpr std::string_view required[] = {
            "schema_version", "pack_id", "files.materials", "files.biomes", "files.fields", "files.features",
            "files.worlds", "files.backgrounds"};
        for (const auto key: required) {
            if (std::ranges::none_of(document->m_entries, [key](const Entry &entry) { return entry.m_key == key; })) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(diagnostics, origin, key, "Required manifest field is missing", EConfigErrc::missing_value));
            }
        }
        const Layer layers[] = {toLayer(*document, std::string{origin})};
        auto resolved = resolve<PackManifestRecord>(layers, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved);
        const auto &record = resolved->m_value;
        if (not validName(record.m_pack_id)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent, reject(diagnostics, origin, "pack_id", "Expected a lowercase stable pack name"));
        }
        ContentPackManifest manifest{
            .m_pack_id = record.m_pack_id,
            .m_files = {
                record.m_materials, record.m_biomes, record.m_fields, record.m_features, record.m_worlds,
                record.m_backgrounds}};
        for (std::size_t index = 0u; index < manifest.m_files.size(); ++index) {
            const auto &path = manifest.m_files[index];
            const auto key = required[index + 2u];
            const auto entry = std::ranges::find(document->m_entries, key, &Entry::m_key);
            manifest.m_file_locations[index] = entry->m_location;
            if (not validTexturePath(path) or not path.ends_with(".toml") or path == "pack.toml") {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, entry->m_location, key,
                                       "Expected a distinct pack-relative TOML file without dot segments"));
            }
            for (std::size_t previous = 0u; previous < index; ++previous) {
                if (path == manifest.m_files[previous]) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, entry->m_location, key,
                                           "Manifest file is listed more than once", EConfigErrc::duplicate_key));
                }
            }
        }
        return manifest;
    }

    Expected<ContentPackSource> loadContentPackSource(
        const std::filesystem::path &root, Diagnostics &diagnostics, const IContext *const context) {
        const auto cancelled = [&]() -> std::error_code { return context ? context->error() : std::error_code{}; };
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        std::error_code error{};
        const auto canonical_root = std::filesystem::canonical(root, error);
        if (error) {
            diagnostics.add(root.string(), "pack", "Cannot resolve pack root", error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        }
        const auto origin = (canonical_root / "pack.toml").string();
        const auto read = [&](const std::string &relative, const std::string_view key, const std::string_view location,
                              const std::size_t limit) -> Expected<ContentSourceDocument> {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
            std::error_code file_error{};
            const auto path = std::filesystem::canonical(canonical_root / relative, file_error);
            if (file_error) {
                diagnostics.add(
                    std::string{location}, std::string{key}, "Cannot resolve pack file " + relative, file_error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, file_error);
            }
            const auto within = path.lexically_relative(canonical_root);
            if (within.empty() or within.is_absolute() or *within.begin() == "..") {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(diagnostics, location, key, "Resolved file escapes the pack root"));
            }
            const bool regular = std::filesystem::is_regular_file(path, file_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, file_error);
            if (not regular) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(diagnostics, location, key, "Pack entry must be a regular file"));
            }
            const auto size = std::filesystem::file_size(path, file_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, file_error);
            if (size > limit) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(diagnostics, location, key, "Pack file exceeds its byte limit", EConfigErrc::out_of_range));
            }
            ContentSourceDocument source{.m_path = path};
            if (size == 0u) {
                return source;
            }
            auto mapped = io::mapFile(path);
            // Core mapping translates allocator failure; this application's
            // allocation policy is fatal, including at this I/O boundary.
            if (not mapped and mapped.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            if (not mapped) {
                diagnostics.add(
                    std::string{location}, std::string{key}, "Cannot map pack file " + relative, mapped.error());
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, mapped);
            }
            if (mapped->size() != size) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::resource_unavailable_try_again);
            }
            source.m_bytes.assign(mapped->c_str(), mapped->size());
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
            return source;
        };
        auto manifest_source = read("pack.toml", "pack", origin, kMaxPackManifestBytes);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, manifest_source);
        auto manifest = decodePackManifest(manifest_source->m_bytes, origin, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, manifest);
        ContentPackSource source{.m_manifest = std::move(*manifest)};
        constexpr std::string_view keys[] = {
            "files.materials", "files.biomes", "files.fields", "files.features", "files.worlds", "files.backgrounds"};
        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            const auto &location = source.m_manifest.m_file_locations[index];
            auto document = read(source.m_manifest.m_files[index], keys[index], location, kMaxMaterialDocumentBytes);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
            if (std::filesystem::equivalent(document->m_path, manifest_source->m_path, error)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location, keys[index], "A content document aliases pack.toml",
                                       EConfigErrc::duplicate_key));
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
            for (std::size_t previous = 0u; previous < index; ++previous) {
                if (std::filesystem::equivalent(document->m_path, source.m_documents[previous].m_path, error)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location, keys[index],
                                           "Manifest entries resolve to the same file", EConfigErrc::duplicate_key));
                }
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
            }
            source.m_documents[index] = std::move(*document);
        }
        source.m_manifest_source = std::move(*manifest_source);
        return source;
    }

    Expected<ContentCatalog> decodeMaterialCatalog(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) try {
        auto groups = groupRecords(bytes, origin, "materials", kMaxContentMaterials, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, groups);

        ContentCatalog catalog{};
        for (const auto &[key, group]: *groups) {
            Diagnostics local{64u};
            auto material = compileMaterial(key, group, origin, local);
            // Resolution uses local schema names. Restore the complete authored
            // key on every diagnostic before propagating its original error.
            appendDiagnostics(diagnostics, local, "materials." + key + ".");
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, material);
            if (const auto *const previous = catalog.findMaterial(material->m_numeric_id)) {
                const auto id = std::ranges::find(group.m_entries, "numeric_id", &Entry::m_key);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, id->m_location, "materials." + key + ".numeric_id",
                        "Numeric ID duplicates materials." + previous->m_key, EConfigErrc::duplicate_key));
            }
            catalog.m_materials.push_back(std::move(*material));
        }
        return catalog;
    } catch (const std::bad_alloc &) {
        // Allocation failure belongs to the application's fatal exception boundary.
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }

    Expected<Array<WorldProfileDefinition>> decodeWorldProfiles(
        const std::string_view bytes, const std::string_view origin, const ContentCatalog &materials,
        const std::span<const FieldDefinition> fields, const std::span<const BiomeDefinition> biomes,
        const std::span<const FeatureDefinition> features, Diagnostics &diagnostics) try {
        auto groups = groupRecords(bytes, origin, "worlds", kMaxContentWorlds, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, groups);
        Array<WorldProfileDefinition> profiles{};
        profiles.reserve(groups->size());
        for (const auto &[key, group]: *groups) {
            auto profile = compileWorldProfile(key, group, materials, fields, biomes, features, origin, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, profile);
            profiles.push_back(std::move(*profile));
        }
        return profiles;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }

    Expected<Array<FeatureDefinition>> decodeFeatureDefinitions(
        const std::string_view bytes, const std::string_view origin, const ContentCatalog &materials,
        Diagnostics &diagnostics) try {
        auto groups = groupRecords(bytes, origin, "features", kMaxContentFeatures, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, groups);
        Array<FeatureDefinition> features{};
        features.reserve(groups->size());
        for (const auto &[key, group]: *groups) {
            auto feature = compileFeature(key, group, materials, origin, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, feature);
            features.push_back(std::move(*feature));
        }
        return features;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }

    Expected<Array<BiomeDefinition>> decodeBiomeDefinitions(
        const std::string_view bytes, const std::string_view origin, const ContentCatalog &materials,
        const std::span<const FieldDefinition> fields, Diagnostics &diagnostics) try {
        auto groups = groupRecords(bytes, origin, "biomes", kMaxContentBiomes, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, groups);
        Array<BiomeDefinition> biomes{};
        biomes.reserve(groups->size());
        for (const auto &[key, group]: *groups) {
            auto biome = compileBiome(key, group, materials, fields, origin, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, biome);
            biomes.push_back(std::move(*biome));
        }
        u32 blob_count{};
        for (const auto &biome: biomes) {
            for (const auto &deposit: biome.m_deposits) {
                blob_count += deposit.m_blob_count;
                if (blob_count > 4096u) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, origin, "biomes", "Aggregate blob count exceeds 4096",
                                           EConfigErrc::out_of_range));
                }
            }
        }
        return biomes;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }

    Expected<Array<FieldDefinition>> decodeFieldDefinitions(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) try {
        auto groups = groupRecords(bytes, origin, "fields", kMaxContentFields, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, groups);

        Array<FieldDefinition> fields{};
        fields.reserve(groups->size());
        for (const auto &[key, group]: *groups) {
            Diagnostics local{64u};
            auto field = compileField(key, group, origin, local);
            appendDiagnostics(diagnostics, local, "fields." + key + ".");
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, field);
            fields.push_back(std::move(*field));
        }
        return fields;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }
}
