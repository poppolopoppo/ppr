module;
#include "pP/Macros.h"
module game.colony.worldgen;
import game.colony.metric;
import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyWorldPlan, info, none);
    namespace {
        // Length-prefixed UTF-8 components make the stream identity independent
        // of array positions, document paths and ambiguous dotted concatenation.
        class Compiler {
            std::set<u64> m_keys;

        public:
            std::error_code m_error{};
            [[nodiscard]] u64 key(const std::initializer_list<std::string_view> parts) {
                u64 hash = 14695981039346656037ull;
                const auto byte = [&hash](const u8 value) { hash = (hash ^ value) * 1099511628211ull; };
                for (const auto part: parts) {
                    const u64 size = part.size();
                    for (u32 shift = 0u; shift < 64u; shift += 8u) {
                        byte(static_cast<u8>(size >> shift));
                    }
                    for (const char value: part) {
                        byte(static_cast<u8>(value));
                    }
                }
                if (not m_keys.insert(hash).second) {
                    m_error = std::make_error_code(std::errc::invalid_argument);
                }
                return hash;
            }
            template<class T>
            [[nodiscard]] u32 index(const std::span<const T> values, const std::string_view name) {
                for (u32 i = 0u; i < values.size(); ++i) {
                    if (values[i].m_key == name) {
                        return i;
                    }
                }
                // The complete catalog resolves these references during loading.
                // Retain a visible error for an inconsistent supplied catalog.
                m_error = std::make_error_code(std::errc::invalid_argument);
                return 0u;
            }
        };
        [[nodiscard]] sim::EGenerationMatter matter(const EMaterialPhase phase) noexcept {
            switch (phase) {
                case EMaterialPhase::solid: return sim::EGenerationMatter::solid;
                case EMaterialPhase::liquid: return sim::EGenerationMatter::liquid;
                case EMaterialPhase::gas: return sim::EGenerationMatter::gas;
            }
            std::unreachable();
        }
        [[nodiscard]] sim::EGenerationShape shape(const EFeatureShape value) noexcept {
            switch (value) {
                case EFeatureShape::rectangle: return sim::EGenerationShape::rectangle;
                case EFeatureShape::ellipse: return sim::EGenerationShape::ellipse;
                case EFeatureShape::capsule: return sim::EGenerationShape::capsule;
            }
            std::unreachable();
        }
        [[nodiscard]] sim::EGenerationMarker role(const EFeatureMarkerRole value) noexcept {
            switch (value) {
                case EFeatureMarkerRole::start: return sim::EGenerationMarker::start;
                case EFeatureMarkerRole::traversal_room: return sim::EGenerationMarker::traversal_room;
                case EFeatureMarkerRole::sealed_pocket: return sim::EGenerationMarker::sealed_pocket;
                case EFeatureMarkerRole::resource_anchor: return sim::EGenerationMarker::resource_anchor;
                case EFeatureMarkerRole::inert: return sim::EGenerationMarker::inert;
            }
            std::unreachable();
        }
        [[nodiscard]] sim::EGenerationPhase phase(const EWorldRulePhase value) noexcept {
            switch (value) {
                case EWorldRulePhase::deposit: return sim::EGenerationPhase::deposit;
                case EWorldRulePhase::carve: return sim::EGenerationPhase::carve;
                case EWorldRulePhase::stamp: return sim::EGenerationPhase::stamp;
                case EWorldRulePhase::fill: return sim::EGenerationPhase::fill;
            }
            std::unreachable();
        }
        void compileBiomes(Compiler &compiler, const ContentCatalog &catalog, sim::GenerationRecipe &recipe) {
            for (const auto &source: catalog.biomes()) {
                sim::GenerationBiome biome{
                    .m_key = compiler.key({"biomes", source.m_key}),
                    .m_temperature_field = compiler.index(catalog.fields(), source.m_temperature_field),
                    .m_temperature_base_k = source.m_temperature_base_k,
                    .m_temperature_variation_k = source.m_temperature_variation_k,
                    .m_lining_material = source.m_lining_material_id,
                    .m_lining_thickness_fine = source.m_lining_thickness_fine,
                    .m_relaxation_passes = source.m_relaxation_passes,
                    .m_relaxation_adoption = source.m_relaxation_adoption,
                    .m_sprinkle_probability = source.m_sprinkle_probability};
                if (source.m_pore_field) {
                    biome.m_pore_field = compiler.index(catalog.fields(), *source.m_pore_field);
                }
                biome.m_pore_probability = source.m_pore_probability;
                biome.m_pore_radius_min_m = source.m_pore_radius_min_m;
                biome.m_pore_radius_max_m = source.m_pore_radius_max_m;
                biome.m_pore_affinity_min = source.m_pore_affinity_min;
                biome.m_pore_passes = source.m_pore_passes;
                biome.m_finish_passes = source.m_finish_passes;
                biome.m_erosion_passes = source.m_erosion_passes;
                biome.m_finish_adoption = source.m_finish_adoption;
                biome.m_soil_material = source.m_soil_material_id;
                biome.m_weathered_material = source.m_weathered_material_id;
                biome.m_soil_depth_m = source.m_soil_depth_m;
                biome.m_weathered_depth_m = source.m_weathered_depth_m;
                biome.m_formation_material = source.m_formation_material_id;
                biome.m_rind_material = source.m_rind_material_id;
                biome.m_formation_probability = source.m_formation_probability;
                biome.m_formation_length_m = source.m_formation_length_m;
                for (const auto &band: source.m_composition) {
                    sim::GenerationBand target{
                        .m_key = compiler.key({"biomes", source.m_key, "composition", band.m_key}),
                        .m_order = band.m_order,
                        .m_field = compiler.index(catalog.fields(), band.m_field),
                        .m_depth_min = band.m_depth_min_normalized,
                        .m_depth_max = band.m_depth_max_normalized};
                    target.m_bed_period_m = band.m_bed_period_m;
                    target.m_bed_warp_m = band.m_bed_warp_m;
                    target.m_bed_dip = band.m_bed_dip;
                    for (const auto &choice: band.m_materials) {
                        target.m_choices.push_back(
                            {compiler.key(
                                 {"biomes", source.m_key, "composition", band.m_key, "materials", choice.m_key}),
                                choice.m_material_id, choice.m_weight});
                    }
                    biome.m_bands.push_back(std::move(target));
                }
                for (const auto &deposit: source.m_deposits) {
                    biome.m_deposits.push_back(
                        {compiler.key({"biomes", source.m_key, "deposits", deposit.m_key}), deposit.m_order,
                            compiler.index(catalog.fields(), deposit.m_field), deposit.m_material_id,
                            deposit.m_host_material_id, deposit.m_depth_min_normalized, deposit.m_depth_max_normalized,
                            deposit.m_threshold_min, deposit.m_threshold_max, deposit.m_shape, deposit.m_stage,
                            deposit.m_blob_count, deposit.m_radius_min_m, deposit.m_radius_max_m, deposit.m_stretch_min,
                            deposit.m_stretch_max, deposit.m_distortion, deposit.m_core_coverage,
                            deposit.m_fringe_coverage, deposit.m_sprinkle_probability,
                            deposit.m_orientation_min_degrees, deposit.m_orientation_max_degrees});
                }
                recipe.m_biomes.push_back(std::move(biome));
            }
        }
        void compileFeatures(Compiler &compiler, const ContentCatalog &catalog, sim::GenerationRecipe &recipe) {
            for (const auto &source: catalog.features()) {
                const auto &bounds = source.m_reservation;
                sim::GenerationFeature feature{
                    .m_key = compiler.key({"features", source.m_key}),
                    .m_bounds = {bounds.m_min_x_fine, bounds.m_min_y_fine, bounds.m_max_x_fine, bounds.m_max_y_fine},
                    .m_allow_reflection = source.m_allow_horizontal_reflection};
                for (const auto &operation: source.m_operations) {
                    feature.m_operations.push_back(
                        {compiler.key({"features", source.m_key, "operations", operation.m_key}), operation.m_order,
                            shape(operation.m_shape), operation.m_action == EFeatureAction::carve, operation.m_x_fine,
                            operation.m_y_fine, operation.m_end_x_fine, operation.m_end_y_fine, operation.m_width_fine,
                            operation.m_height_fine, operation.m_radius_m, operation.m_material_id});
                }
                for (const auto &marker: source.m_markers) {
                    feature.m_markers.push_back(
                        {compiler.key({"features", source.m_key, "markers", marker.m_key}), role(marker.m_role),
                            marker.m_x_fine, marker.m_y_fine});
                }
                recipe.m_features.push_back(std::move(feature));
            }
        }
        [[nodiscard]] sim::GenerationRule compileRule(
            Compiler &compiler, const ContentCatalog &catalog, const WorldProfileDefinition &profile,
            const WorldRuleDefinition &source) {
            sim::GenerationRule rule{
                .m_key = compiler.key({"worlds", profile.m_key, "rules", source.m_key}),
                .m_order = source.m_order,
                .m_phase = phase(source.m_phase)};
            std::visit(
                [&](const auto &value) {
                    using T = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, WorldCaveRule>) {
                        rule.m_parameters = sim::GenerationCave{
                            compiler.index(catalog.biomes(), value.m_biome),
                            compiler.index(catalog.fields(), value.m_field), value.m_depth_min_normalized,
                            value.m_depth_max_normalized, value.m_threshold_min, value.m_threshold_max};
                    } else if constexpr (std::is_same_v<T, WorldWormRule>) {
                        rule.m_parameters = sim::GenerationWorm{
                            compiler.index(catalog.biomes(), value.m_biome), value.m_count,
                            source.m_kind == EWorldRuleKind::carve_worm, value.m_depth_min_normalized,
                            value.m_depth_max_normalized, value.m_length_min_m, value.m_length_max_m,
                            value.m_radius_min_m, value.m_radius_max_m, value.m_turn_probability, value.m_material_id,
                            value.m_host_material_id, value.m_turn_smoothing, value.m_radius_variation,
                            value.m_chamber_probability, value.m_chamber_scale};
                    } else if constexpr (std::is_same_v<T, WorldConnectionRule>) {
                        rule.m_parameters = sim::GenerationConnection{
                            value.m_radius_m, value.m_extra_links, value.m_natural_anchors, value.m_wells,
                            value.m_branches, value.m_roughness_m};
                    } else if constexpr (std::is_same_v<T, WorldFeatureRule>) {
                        rule.m_parameters = sim::GenerationPlacement{
                            compiler.index(catalog.features(), value.m_feature),
                            compiler.index(catalog.biomes(), value.m_biome), value.m_min_count, value.m_max_count,
                            value.m_attempts, value.m_priority, value.m_depth_min_normalized,
                            value.m_depth_max_normalized, value.m_spacing_m, value.m_reflect};
                    } else {
                        const u32 placement_index = compiler.index(
                            std::span<const WorldRuleDefinition>{profile.m_rules}, value.m_feature_rule);
                        const auto *const placement = std::get_if<WorldFeatureRule>(
                            &profile.m_rules[placement_index].m_parameters);
                        const auto *const material = catalog.findMaterial(value.m_material_id);
                        if (placement == nullptr or material == nullptr) {
                            compiler.m_error = std::make_error_code(std::errc::invalid_argument);
                            return;
                        }
                        const u32 feature_index = compiler.index(catalog.features(), placement->m_feature);
                        rule.m_parameters = sim::GenerationFill{
                            placement_index,
                            compiler.index(
                                std::span<const FeatureMarker>{catalog.features()[feature_index].m_markers},
                                value.m_marker),
                            value.m_material_id, material->m_phase == EMaterialPhase::gas, value.m_temperature_k,
                            value.m_bulk_density_kg_m3, value.m_fill_height_m};
                    }
                },
                source.m_parameters);
            return rule;
        }
    }

    Expected<sim::GenerationPlan> compileWorldPlan(
        const ContentCatalog &catalog, const std::string_view profile_key, const u64 seed) {
        const auto *const profile = catalog.findWorld(profile_key);
        if (not catalog.contentFingerprint() or profile == nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyWorldPlan, std::errc::invalid_argument);
        }
        Compiler compiler;
        sim::GenerationRecipe recipe;
        for (const auto &source: catalog.materials()) {
            recipe.m_materials.push_back(
                {source.m_numeric_id, source.m_representation == EMaterialRepresentation::terrain,
                    source.m_diggable.value_or(false), matter(source.m_phase), source.m_reference_density_kg_m3,
                    compiler.key({"materials", source.m_key})});
        }
        for (const auto &source: catalog.fields()) {
            recipe.m_fields.push_back({compiler.key({"fields", source.m_key}), source.m_noise});
        }
        compileBiomes(compiler, catalog, recipe);
        compileFeatures(compiler, catalog, recipe);

        recipe.m_cell_size_m = 1.0 / kFineCellsPerMetre;
        recipe.m_reference_thickness_m = 1.0;
        recipe.m_boundary_material = profile->m_default_wall_material_id;
        recipe.m_boundary_fine = profile->m_boundary_fine;
        recipe.m_min_sites = profile->m_min_sites;
        recipe.m_max_sites = profile->m_max_sites;
        recipe.m_site_attempts = profile->m_site_attempts;
        recipe.m_site_spacing_m = profile->m_site_spacing_m;
        recipe.m_boundary_warp_amplitude_m = profile->m_boundary_warp_amplitude_m;
        recipe.m_warp_x_field = compiler.index(catalog.fields(), profile->m_boundary_warp_x_field);
        recipe.m_warp_y_field = compiler.index(catalog.fields(), profile->m_boundary_warp_y_field);
        recipe.m_start_biome = compiler.index(catalog.biomes(), profile->m_start_biome);
        recipe.m_start_feature = compiler.index(catalog.features(), profile->m_start_feature);
        recipe.m_start_marker = compiler.index(
            std::span<const FeatureMarker>{catalog.features()[recipe.m_start_feature].m_markers},
            profile->m_start_marker);
        recipe.m_start_x_fine = profile->m_start_x_fine;
        recipe.m_start_y_fine = profile->m_start_y_fine;
        recipe.m_open_surface = profile->m_surface_field.has_value();
        if (recipe.m_open_surface) {
            recipe.m_surface_field = compiler.index(catalog.fields(), *profile->m_surface_field);
            recipe.m_bedrock_field = compiler.index(catalog.fields(), *profile->m_bedrock_field);
            recipe.m_surface_elevation_m = profile->m_surface_elevation_m;
            recipe.m_surface_amplitude_m = profile->m_surface_amplitude_m;
            recipe.m_bedrock_thickness_m = profile->m_bedrock_thickness_m;
            recipe.m_bedrock_variation_m = profile->m_bedrock_variation_m;
            recipe.m_bedrock_material = profile->m_bedrock_material_id;
            recipe.m_start_depth_m = profile->m_start_depth_m;
        }
        recipe.m_start_clearance_m = profile->m_start_clearance_m;
        recipe.m_start_route_width_m = profile->m_start_route_width_m;
        // Actual player body half extents; authored profiles cannot resize actors.
        recipe.m_actor_half_width_m = 0.4;
        recipe.m_actor_half_height_m = 0.9;
        for (const auto &source: profile->m_biomes) {
            recipe.m_constraints.push_back(
                {compiler.key({"worlds", profile->m_key, "biomes", source.m_key}),
                    compiler.index(catalog.biomes(), source.m_biome), source.m_required_count, source.m_weight,
                    source.m_depth_min_normalized, source.m_depth_max_normalized, source.m_distance_min_m,
                    source.m_distance_max_m});
        }
        for (const auto &source: profile->m_start_resources) {
            recipe.m_resources.push_back(
                {compiler.key({"worlds", profile->m_key, "resources", source.m_key}),
                    compiler.index(
                        std::span<const FeatureMarker>{catalog.features()[recipe.m_start_feature].m_markers},
                        source.m_marker),
                    source.m_minimum_fine_cells, source.m_material_id, source.m_max_distance_m});
        }
        for (const auto &source: profile->m_rules) {
            recipe.m_rules.push_back(compileRule(compiler, catalog, *profile, source));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyWorldPlan, compiler.m_error);
        return sim::compileGenerationPlan(std::move(recipe), seed);
    }
}
