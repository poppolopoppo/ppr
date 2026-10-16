module;
#include "pP/Macros.h"

module engine.sim;
import :worldgen.plan;
import :element;
import :chunk_grid;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldPlan, info, none)

    // ------------------------------------------------------------------
    // plan helpers
    // ------------------------------------------------------------------

    namespace {
        [[nodiscard]] u64 mix(const u64 value) noexcept {
            u64 state = value;
            state = (state ^ (state >> 30u)) * 0xbf58476d1ce4e5b9ull;
            state = (state ^ (state >> 27u)) * 0x94d049bb133111ebull;
            return state ^ (state >> 31u);
        }

        [[nodiscard]] bool finiteRange(const double value, const double low, const double high) noexcept {
            return std::isfinite(value) and value >= low and value <= high;
        }

        [[nodiscard]] bool interval(
            const double low, const double high, const double minimum, const double maximum) noexcept {
            return finiteRange(low, minimum, maximum) and finiteRange(high, minimum, maximum) and low <= high;
        }

        [[nodiscard]] bool positive(const double value, const double maximum) noexcept {
            return std::isfinite(value) and value > 0.0 and value <= maximum;
        }

        template<class T>
        [[nodiscard]] bool validWeights(const Array<T> &values) noexcept {
            double total = 0.0;
            for (const auto &value: values) {
                const double next = total + value.m_weight;
                if (not positive(value.m_weight, (std::numeric_limits<double>::max)()) or
                    not std::isfinite(next) or
                    next <= total) {
                    return false;
                }
                total = next;
            }

            double cumulative = 0.0, previous = 0.0;
            for (const auto &value: values) {
                cumulative += value.m_weight;
                const double fraction = cumulative / total;
                if (fraction <= previous) {
                    return false;
                }
                previous = fraction;
            }
            return true;
        }

        template<class T>
        [[nodiscard]] bool uniqueKeys(const Array<T> &values) noexcept {
            for (std::size_t i = 0u; i < values.size(); ++i) {
                for (std::size_t j = 0u; j < i; ++j) {
                    if (values[i].m_key == values[j].m_key) {
                        return false;
                    }
                }
            }
            return true;
        }

        template<class T>
        [[nodiscard]] bool ordered(const Array<T> &values) noexcept {
            for (std::size_t i = 1u; i < values.size(); ++i) {
                if (values[i - 1u].m_order >= values[i].m_order) {
                    return false;
                }
            }
            return uniqueKeys(values);
        }

        [[nodiscard]] const GenerationMaterial *material(const GenerationRecipe &recipe, const u16 id) noexcept {
            for (const auto &entry: recipe.m_materials) {
                if (entry.m_id == id) {
                    return &entry;
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool terrain(const GenerationRecipe &recipe, const u16 id) noexcept {
            const auto *const entry = material(recipe, id);
            return entry != nullptr and entry->m_terrain;
        }

        [[nodiscard]] bool activeBiome(const GenerationRecipe &recipe, const u32 index) noexcept {
            return std::ranges::any_of(
                recipe.m_constraints, [index](const auto &entry) { return entry.m_biome == index; });
        }

        [[nodiscard]] std::error_code validateCounts(const GenerationRecipe &recipe) {
            if (recipe.m_materials.empty() or
                recipe.m_materials.size() > kMaxGenerationMaterials or
                recipe.m_fields.empty() or
                recipe.m_fields.size() > kMaxGenerationFields or
                recipe.m_biomes.empty() or
                recipe.m_biomes.size() > kMaxGenerationBiomes or
                recipe.m_features.empty() or
                recipe.m_features.size() > kMaxGenerationFeatures or
                recipe.m_constraints.empty() or
                recipe.m_constraints.size() > kMaxGenerationBiomes or
                recipe.m_rules.size() > kMaxGenerationRules or
                recipe.m_resources.size() > 8u) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::value_too_large);
            }

            if (not uniqueKeys(recipe.m_fields) or
                not uniqueKeys(recipe.m_biomes) or
                not uniqueKeys(recipe.m_features) or
                not uniqueKeys(recipe.m_constraints) or
                not uniqueKeys(recipe.m_rules) or
                not uniqueKeys(recipe.m_resources)) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
            }
            return {};
        }

        [[nodiscard]] std::error_code validateMaterials(const GenerationRecipe &recipe) {
            for (std::size_t i = 0u; i < recipe.m_materials.size(); ++i) {
                const auto &entry = recipe.m_materials[i];
                if (entry.m_id == 0u or
                    entry.m_id == 65535u or
                    asset::Element::phase_domain.validate(entry.m_matter) or
                    (entry.m_terrain and entry.m_matter != EGenerationMatter::solid) or
                    asset::Element::density_domain.validate(entry.m_reference_density_kg_m3)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }

                for (std::size_t j = 0u; j < i; ++j) {
                    if (recipe.m_materials[j].m_id == entry.m_id) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code validateBiomes(const GenerationRecipe &recipe) {
            u64 blob_count = 0u;
            for (const auto &biome: recipe.m_biomes) {
                if (biome.m_temperature_field >= recipe.m_fields.size() or
                    not positive(
                        biome.m_temperature_base_k, static_cast<double>((std::numeric_limits<float>::max)()) / 2.0) or
                    not finiteRange(biome.m_temperature_variation_k, 0.0, biome.m_temperature_base_k) or
                    biome.m_temperature_variation_k == biome.m_temperature_base_k or
                    biome.m_bands.empty() or
                    biome.m_bands.size() > 128u or
                    biome.m_deposits.size() > 128u or
                    biome.m_lining_thickness_fine > 3u or
                    biome.m_pore_field >= recipe.m_fields.size() or
                    not finiteRange(biome.m_pore_probability, 0.0, 0.2) or
                    (biome.m_pore_probability > 0.0 and
                        biome.m_pore_radius_max_m > kMaxGenerationPoreRadiusFine * recipe.m_cell_size_m) or
                    not interval(biome.m_pore_radius_min_m, biome.m_pore_radius_max_m, 0.0, 3.0) or
                    not finiteRange(biome.m_pore_affinity_min, -1.0, 1.0) or
                    not finiteRange(biome.m_finish_adoption, 0.0, 0.3) or
                    not finiteRange(biome.m_soil_depth_m, 0.0, 12.0) or
                    not finiteRange(biome.m_weathered_depth_m, 0.0, 30.0) or
                    not finiteRange(biome.m_formation_probability, 0.0, 0.2) or
                    not finiteRange(biome.m_formation_length_m, 0.0, 8.0) or
                    (biome.m_soil_depth_m > 0.0 and not terrain(recipe, biome.m_soil_material)) or
                    (biome.m_weathered_depth_m > 0.0 and not terrain(recipe, biome.m_weathered_material)) or
                    (biome.m_formation_probability > 0.0 and not terrain(recipe, biome.m_formation_material)) or
                    (biome.m_rind_material != 0u and not terrain(recipe, biome.m_rind_material)) or
                    not finiteRange(biome.m_relaxation_adoption, 0.0, 1.0) or
                    not finiteRange(biome.m_sprinkle_probability, 0.0, 1.0) or
                    ((biome.m_lining_material == 0u) != (biome.m_lining_thickness_fine == 0u)) or
                    (biome.m_lining_material != 0u and not terrain(recipe, biome.m_lining_material)) or
                    not ordered(biome.m_bands) or
                    not ordered(biome.m_deposits)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }

                double previous_depth = 0.0;
                for (const auto &band: biome.m_bands) {
                    if (band.m_field >= recipe.m_fields.size() or
                        not finiteRange(band.m_bed_period_m, 0.0, 256.0) or
                        not finiteRange(band.m_bed_warp_m, 0.0, 64.0) or
                        not finiteRange(band.m_bed_dip, -0.5, 0.5) or
                        (band.m_bed_period_m == 0.0 and (band.m_bed_warp_m != 0.0 or band.m_bed_dip != 0.0)) or
                        not interval(band.m_depth_min, band.m_depth_max, 0.0, 1.0) or
                        band.m_depth_min != previous_depth or
                        band.m_depth_min == band.m_depth_max or
                        band.m_choices.empty() or
                        band.m_choices.size() > kMaxGenerationMaterials or
                        not uniqueKeys(band.m_choices) or
                        std::ranges::none_of(
                            band.m_choices, [](const auto &choice) { return choice.m_material != 0u; }) or
                        not validWeights(band.m_choices)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                    for (const auto &choice: band.m_choices) {
                        if (choice.m_material != 0u and not terrain(recipe, choice.m_material)) {
                            PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                        }
                    }
                    previous_depth = band.m_depth_max;
                }

                if (previous_depth != 1.0) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }

                for (const auto &deposit: biome.m_deposits) {
                    if (deposit.m_field >= recipe.m_fields.size() or
                        not terrain(recipe, deposit.m_material) or
                        not terrain(recipe, deposit.m_host) or
                        not interval(deposit.m_depth_min, deposit.m_depth_max, 0.0, 1.0) or
                        not interval(deposit.m_threshold_min, deposit.m_threshold_max, -1.0, 1.0) or
                        deposit.m_shape > EGenerationDepositShape::blob or
                        deposit.m_stage > EGenerationDepositStage::detail or
                        not finiteRange(deposit.m_core_coverage, 0.0, 1.0) or
                        not finiteRange(deposit.m_fringe_coverage, 0.0, 1.0) or
                        deposit.m_core_coverage < deposit.m_fringe_coverage or
                        (deposit.m_stage == EGenerationDepositStage::bulk and deposit.m_sprinkle_probability != 0.0) or
                        not finiteRange(deposit.m_sprinkle_probability, 0.0, 0.02)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                    if (deposit.m_shape == EGenerationDepositShape::blob) {
                        if (deposit.m_blob_count == 0u or
                            deposit.m_blob_count > 4096u or
                            deposit.m_depth_min == deposit.m_depth_max or
                            not interval(deposit.m_radius_min_m, deposit.m_radius_max_m, 0.0, 256.0) or
                            deposit.m_radius_min_m == 0.0 or
                            not interval(deposit.m_stretch_min, deposit.m_stretch_max, 0.1, 16.0) or
                            not interval(
                                deposit.m_orientation_min_degrees, deposit.m_orientation_max_degrees, 0.0, 360.0) or
                            not finiteRange(deposit.m_distortion, 0.0, 0.45) or
                            deposit.m_radius_max_m *
                                    std::max(1.0, deposit.m_stretch_max) *
                                    (1.0 + deposit.m_distortion) *
                                    1.2 >
                                kWorldEdge * recipe.m_cell_size_m) {
                            PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                        }
                        blob_count += deposit.m_blob_count;
                    } else if (deposit.m_blob_count != 0u or
                               deposit.m_radius_min_m != 0.0 or
                               deposit.m_radius_max_m != 0.0 or
                               deposit.m_stretch_min != 1.0 or
                               deposit.m_stretch_max != 1.0 or
                               deposit.m_distortion != 0.0 or
                               deposit.m_orientation_min_degrees != 0.0 or
                               deposit.m_orientation_max_degrees != 360.0 or
                               deposit.m_core_coverage != 1.0 or
                               deposit.m_fringe_coverage != 1.0 or
                               deposit.m_sprinkle_probability != 0.0) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }
            }

            if (blob_count > 4096u) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::value_too_large);
            }

            return {};
        }

        [[nodiscard]] bool inside(const GenerationBounds &bounds, const i64 x, const i64 y) noexcept {
            return x >= bounds.m_min_x and x < bounds.m_max_x and y >= bounds.m_min_y and y < bounds.m_max_y;
        }

        [[nodiscard]] bool validOperation(
            const GenerationRecipe &recipe, const GenerationFeature &feature,
            const GenerationOperation &operation) noexcept {
            if (operation.m_shape > EGenerationShape::capsule or
                not inside(feature.m_bounds, operation.m_x, operation.m_y) or
                (not operation.m_carve and not terrain(recipe, operation.m_material))) {
                return false;
            }
            if (operation.m_shape != EGenerationShape::capsule) {
                return operation.m_width > 0u and
                       operation.m_height > 0u and
                       inside(
                           feature.m_bounds, static_cast<i64>(operation.m_x) + operation.m_width - 1,
                           static_cast<i64>(operation.m_y) + operation.m_height - 1);
            }
            if (not positive(operation.m_radius_m, kWorldEdge * recipe.m_cell_size_m)) {
                return false;
            }
            const double radius = operation.m_radius_m * (1.0 / recipe.m_cell_size_m);
            return std::min(operation.m_x, operation.m_end_x) - radius >= feature.m_bounds.m_min_x and
                   std::min(operation.m_y, operation.m_end_y) - radius >= feature.m_bounds.m_min_y and
                   std::max(operation.m_x, operation.m_end_x) + radius <= feature.m_bounds.m_max_x and
                   std::max(operation.m_y, operation.m_end_y) + radius <= feature.m_bounds.m_max_y;
        }

        [[nodiscard]] std::error_code validateFeatures(const GenerationRecipe &recipe) {
            for (const auto &feature: recipe.m_features) {
                const auto &bounds = feature.m_bounds;
                if (bounds.m_min_x >= bounds.m_max_x or
                    bounds.m_min_y >= bounds.m_max_y or
                    bounds.m_min_x < -static_cast<i32>(kWorldEdge) or
                    bounds.m_min_y < -static_cast<i32>(kWorldEdge) or
                    bounds.m_max_x > static_cast<i32>(kWorldEdge) or
                    bounds.m_max_y > static_cast<i32>(kWorldEdge) or
                    static_cast<i64>(bounds.m_max_x) - bounds.m_min_x > kWorldEdge or
                    static_cast<i64>(bounds.m_max_y) - bounds.m_min_y > kWorldEdge or
                    feature.m_operations.empty() or
                    feature.m_operations.size() > 128u or
                    feature.m_markers.size() > 64u or
                    not ordered(feature.m_operations) or
                    not uniqueKeys(feature.m_markers)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }

                for (const auto &operation: feature.m_operations) {
                    if (not validOperation(recipe, feature, operation)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }

                for (const auto &marker: feature.m_markers) {
                    if (marker.m_role > EGenerationMarker::inert or not inside(bounds, marker.m_x, marker.m_y)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code validateLayout(const GenerationRecipe &recipe) {
            const double world_m = kWorldEdge * recipe.m_cell_size_m;
            if (not terrain(recipe, recipe.m_boundary_material) or
                recipe.m_boundary_fine == 0u or
                recipe.m_boundary_fine >= kWorldEdge / 2u or
                recipe.m_min_sites == 0u or
                recipe.m_min_sites > recipe.m_max_sites or
                recipe.m_max_sites > kMaxGenerationSites or
                recipe.m_site_attempts == 0u or
                recipe.m_site_attempts > 4096u or
                not positive(recipe.m_site_spacing_m, world_m) or
                recipe.m_site_spacing_m * recipe.m_site_spacing_m == 0.0 or
                not finiteRange(recipe.m_boundary_warp_amplitude_m, 0.0, world_m) or
                recipe.m_warp_x_field >= recipe.m_fields.size() or
                recipe.m_warp_y_field >= recipe.m_fields.size()) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
            }

            u64 required = 0u;
            for (std::size_t i = 0u; i < recipe.m_constraints.size(); ++i) {
                const auto &constraint = recipe.m_constraints[i];
                if (constraint.m_biome >= recipe.m_biomes.size() or
                    not interval(constraint.m_depth_min, constraint.m_depth_max, 0.0, 1.0) or
                    not interval(constraint.m_distance_min_m, constraint.m_distance_max_m, 0.0, world_m * 2.0)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }
                for (std::size_t j = 0u; j < i; ++j) {
                    if (recipe.m_constraints[j].m_biome == constraint.m_biome) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }
                required += constraint.m_required_count;
            }

            if (required > recipe.m_max_sites or not validWeights(recipe.m_constraints)) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
            }

            return {};
        }

        [[nodiscard]] bool validRule(const GenerationRecipe &recipe, const GenerationRule &rule) noexcept {
            return std::visit(
                [&](const auto &value) {
                    using T = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, GenerationCave>) {
                        return rule.m_phase == EGenerationPhase::carve and
                               activeBiome(recipe, value.m_biome) and
                               value.m_field < recipe.m_fields.size() and
                               interval(value.m_depth_min, value.m_depth_max, 0.0, 1.0) and
                               interval(value.m_threshold_min, value.m_threshold_max, -1.0, 1.0);
                    } else if constexpr (std::is_same_v<T, GenerationWorm>) {
                        return rule.m_phase == (value.m_carve ? EGenerationPhase::carve : EGenerationPhase::deposit) and
                               activeBiome(recipe, value.m_biome) and
                               value.m_count > 0u and
                               value.m_count <= 4096u and
                               interval(value.m_depth_min, value.m_depth_max, 0.0, 1.0) and
                               interval(value.m_length_min_m, value.m_length_max_m, 0.0, 4096.0) and
                               value.m_length_min_m > 0.0 and
                               interval(value.m_radius_min_m, value.m_radius_max_m, 0.0, 256.0) and
                               value.m_radius_min_m > 0.0 and
                               finiteRange(value.m_turn_probability, 0.0, 1.0) and
                               finiteRange(value.m_turn_smoothing, 0.0, 1.0) and
                               finiteRange(value.m_radius_variation, 0.0, 1.0) and
                               finiteRange(value.m_chamber_probability, 0.0, 0.1) and
                               finiteRange(value.m_chamber_scale, 1.0, 4.0) and
                               (value.m_turn_smoothing > 0.0 or (value.m_radius_variation == 0.0 and
                                                                    value.m_chamber_probability == 0.0 and
                                                                    value.m_chamber_scale == 1.0)) and
                               (value.m_carve or (terrain(recipe, value.m_material) and terrain(recipe, value.m_host)));
                    } else if constexpr (std::is_same_v<T, GenerationConnection>) {
                        return rule.m_phase == EGenerationPhase::carve and
                               positive(value.m_radius_m, 256.0) and
                               value.m_extra_links <= 1024u and
                               value.m_natural_anchors <= 128u and
                               value.m_wells <= 32u and
                               value.m_branches <= 64u and
                               finiteRange(value.m_roughness_m, 0.0, 8.0);
                    } else if constexpr (std::is_same_v<T, GenerationPlacement>) {
                        return rule.m_phase == EGenerationPhase::stamp and
                               value.m_feature < recipe.m_features.size() and
                               activeBiome(recipe, value.m_biome) and
                               value.m_min_count <= value.m_max_count and
                               value.m_max_count <= 1024u and
                               value.m_attempts > 0u and
                               value.m_attempts <= 4096u and
                               interval(value.m_depth_min, value.m_depth_max, 0.0, 1.0) and
                               finiteRange(value.m_spacing_m, 0.0, kWorldEdge * recipe.m_cell_size_m) and
                               (not value.m_reflect or recipe.m_features[value.m_feature].m_allow_reflection);
                    } else {
                        if (rule.m_phase != EGenerationPhase::fill or value.m_placement_rule >= recipe.m_rules.size()) {
                            return false;
                        }
                        const auto *const placement = std::get_if<GenerationPlacement>(
                            &recipe.m_rules[value.m_placement_rule].m_parameters);
                        const auto *const entry = material(recipe, value.m_material);
                        if (placement == nullptr or
                            placement->m_feature >= recipe.m_features.size() or
                            entry == nullptr or
                            entry->m_terrain or
                            (value.m_gas != (entry->m_matter == EGenerationMatter::gas)) or
                            not positive(
                                value.m_temperature_k, static_cast<double>((std::numeric_limits<float>::max)())) or
                            not positive(value.m_bulk_density_kg_m3, 1.0e9)) {
                            return false;
                        }
                        const auto &feature = recipe.m_features[placement->m_feature];
                        if (value.m_marker >= feature.m_markers.size() or
                            feature.m_markers[value.m_marker].m_role != EGenerationMarker::sealed_pocket) {
                            return false;
                        }

                        if (value.m_gas) {
                            return not value.m_fill_height_m.has_value();
                        }

                        if (not value.m_fill_height_m or not finiteRange(*value.m_fill_height_m, -4096.0, 4096.0)) {
                            return false;
                        }

                        // Compare with the canonical boundary itself, not a tolerance
                        // which would silently accept neighbouring floating-point values.
                        const double cells_per_metre = 1.0 / recipe.m_cell_size_m;
                        const double cells = std::round(*value.m_fill_height_m * cells_per_metre);
                        return std::isfinite(cells) and
                               cells > feature.m_bounds.m_min_y and
                               cells <= feature.m_bounds.m_max_y and
                               *value.m_fill_height_m == cells / cells_per_metre;
                    }
                },
                rule.m_parameters);
        }

        [[nodiscard]] std::error_code validateRules(const GenerationRecipe &recipe) {
            u64 worm_count = 0u, feature_count = 0u, origin_attempts = 0u;
            for (const auto &biome: recipe.m_biomes) {
                for (const auto &deposit: biome.m_deposits) {
                    origin_attempts += static_cast<u64>(deposit.m_blob_count) * kGenerationBlobOriginAttempts;
                }
            }

            u32 connections = 0u;
            for (std::size_t i = 0u; i < recipe.m_rules.size(); ++i) {
                const auto &rule = recipe.m_rules[i];
                if (not validRule(recipe, rule)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }
                if (i > 0u) {
                    const auto &previous = recipe.m_rules[i - 1u];
                    if (std::tie(previous.m_phase, previous.m_order) >= std::tie(rule.m_phase, rule.m_order)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }
                if (const auto *const worm = std::get_if<GenerationWorm>(&rule.m_parameters)) {
                    worm_count += worm->m_count;
                    origin_attempts += static_cast<u64>(worm->m_count) * kGenerationWormOriginAttempts;
                } else if (const auto *const placement = std::get_if<GenerationPlacement>(&rule.m_parameters)) {
                    feature_count += placement->m_max_count;
                    origin_attempts += static_cast<u64>(placement->m_max_count) * placement->m_attempts;
                } else if (std::holds_alternative<GenerationConnection>(rule.m_parameters)) {
                    ++connections;
                }
            }

            if (worm_count > 4096u or
                feature_count > 1024u or
                connections > 1u or
                origin_attempts > kMaxGenerationOriginAttempts) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::value_too_large);
            }

            return {};
        }

        [[nodiscard]] std::error_code validateStart(const GenerationRecipe &recipe) {
            if (not activeBiome(recipe, recipe.m_start_biome) or
                recipe.m_start_feature >= recipe.m_features.size() or
                recipe.m_start_x_fine >= kWorldEdge or
                recipe.m_start_y_fine >= kWorldEdge or
                not positive(recipe.m_start_clearance_m, kWorldEdge * recipe.m_cell_size_m) or
                not positive(recipe.m_start_route_width_m, kWorldEdge * recipe.m_cell_size_m) or
                not positive(recipe.m_actor_half_width_m, kWorldEdge * recipe.m_cell_size_m) or
                not positive(recipe.m_actor_half_height_m, kWorldEdge * recipe.m_cell_size_m) or
                recipe.m_start_route_width_m < 2.0 * recipe.m_actor_half_width_m or
                recipe.m_start_clearance_m < 2.0 * recipe.m_actor_half_height_m) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
            }

            const auto &feature = recipe.m_features[recipe.m_start_feature];
            const auto &bounds = feature.m_bounds;
            if (recipe.m_start_marker >= feature.m_markers.size() or
                feature.m_markers[recipe.m_start_marker].m_role != EGenerationMarker::start or
                static_cast<i64>(recipe.m_start_x_fine) + bounds.m_min_x < recipe.m_boundary_fine or
                static_cast<i64>(recipe.m_start_y_fine) + bounds.m_min_y < recipe.m_boundary_fine or
                static_cast<i64>(recipe.m_start_x_fine) + bounds.m_max_x > kWorldEdge - recipe.m_boundary_fine or
                static_cast<i64>(recipe.m_start_y_fine) + bounds.m_max_y > kWorldEdge - recipe.m_boundary_fine) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
            }

            for (std::size_t i = 0u; i < recipe.m_resources.size(); ++i) {
                const auto &resource = recipe.m_resources[i];
                const auto *const entry = material(recipe, resource.m_material);
                if (entry == nullptr or
                    not entry->m_terrain or
                    not entry->m_diggable or
                    resource.m_marker >= feature.m_markers.size() or
                    feature.m_markers[resource.m_marker].m_role != EGenerationMarker::resource_anchor or
                    resource.m_minimum_fine_cells == 0u or
                    resource.m_minimum_fine_cells > static_cast<u64>(kWorldEdge) * kWorldEdge or
                    not positive(resource.m_max_distance_m, kWorldEdge * recipe.m_cell_size_m * 2.0)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }
                for (std::size_t j = 0u; j < i; ++j) {
                    if (recipe.m_resources[j].m_marker == resource.m_marker) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }
            }
            return {};
        }
    }

    // ------------------------------------------------------------------
    // deterministic random
    // ------------------------------------------------------------------

    u64 generationStreamSeed(const u64 seed, const u64 domain_key, const u64 ordinal) noexcept {
        return mix(mix(seed ^ 0x7070722e67656e31ull) ^ mix(domain_key) ^ mix(ordinal + 0x9e3779b97f4a7c15ull));
    }
    GenerationRandom::GenerationRandom(const u64 state) noexcept : m_state(state) {
    }

    u64 GenerationRandom::next() noexcept {
        m_state += 0x9e3779b97f4a7c15ull;
        return mix(m_state);
    }
    u64 GenerationRandom::bounded(const u64 exclusive_max) noexcept {
        PPR_ASSERT(exclusive_max > 0u);
        const u64 threshold = (0u - exclusive_max) % exclusive_max;
        u64 value;
        do {
            value = next();
        } while (value < threshold);
        return value % exclusive_max;
    }

    double GenerationRandom::unit() noexcept {
        return static_cast<double>(next() >> 11u) * 0x1.0p-53;
    }
    // ------------------------------------------------------------------
    // compiled plan
    // ------------------------------------------------------------------

    GenerationPlan::GenerationPlan(
        GenerationRecipe recipe, Array<NoiseField> fields, Array<GenerationMaterialLookup> material_lookup,
        const u64 seed) noexcept
        : m_recipe(std::move(recipe)), m_fields(std::move(fields)), m_material_lookup(std::move(material_lookup)),
          m_seed(seed) {
    }

    const GenerationRecipe &GenerationPlan::recipe() const noexcept {
        return m_recipe;
    }

    std::span<const NoiseField> GenerationPlan::fields() const noexcept {
        return m_fields;
    }

    std::span<const GenerationMaterialLookup> GenerationPlan::materialLookup() const noexcept {
        return m_material_lookup;
    }

    u64 GenerationPlan::seed() const noexcept {
        return m_seed;
    }

    u64 GenerationPlan::retainedPayloadBytes() const noexcept {
        u64 bytes = m_fields.capacity() * sizeof(NoiseField) +
                    m_material_lookup.capacity() * sizeof(GenerationMaterialLookup) +
                    m_recipe.m_materials.capacity() * sizeof(GenerationMaterial) +
                    m_recipe.m_fields.capacity() * sizeof(GenerationField) +
                    m_recipe.m_biomes.capacity() * sizeof(GenerationBiome) +
                    m_recipe.m_features.capacity() * sizeof(GenerationFeature) +
                    m_recipe.m_constraints.capacity() * sizeof(GenerationBiomeConstraint) +
                    m_recipe.m_rules.capacity() * sizeof(GenerationRule) +
                    m_recipe.m_resources.capacity() * sizeof(GenerationResource);
        for (const auto &biome: m_recipe.m_biomes) {
            bytes += biome.m_bands.capacity() * sizeof(GenerationBand) +
                     biome.m_deposits.capacity() * sizeof(GenerationDeposit);
            for (const auto &band: biome.m_bands) {
                bytes += band.m_choices.capacity() * sizeof(GenerationChoice);
            }
        }
        for (const auto &feature: m_recipe.m_features) {
            bytes += feature.m_operations.capacity() * sizeof(GenerationOperation) +
                     feature.m_markers.capacity() * sizeof(GenerationMarker);
        }

        return bytes;
    }

    // ------------------------------------------------------------------
    // plan queries
    // ------------------------------------------------------------------

    double generationSurfaceHeight(const GenerationPlan &plan, const double x_m) noexcept {
        const auto &r = plan.recipe();
        return r.m_open_surface ? r.m_surface_elevation_m +
                                      r.m_surface_amplitude_m * static_cast<double>(plan.fields()[r.m_surface_field].sample(x_m, 0.0))
                                : kWorldEdge * r.m_cell_size_m;
    }

    double generationBedrockHeight(const GenerationPlan &plan, const double x_m) noexcept {
        const auto &r = plan.recipe();
        return r.m_open_surface ? r.m_bedrock_thickness_m +
                                      r.m_bedrock_variation_m * static_cast<double>(plan.fields()[r.m_bedrock_field].sample(x_m, 0.0))
                                : r.m_boundary_fine * r.m_cell_size_m;
    }

    double generationDepth(const GenerationPlan &plan, const double x_m, const double y_m) noexcept {
        const double surface = generationSurfaceHeight(plan, x_m);
        return std::clamp(
            (surface - y_m) / (surface - (plan.recipe().m_open_surface ? generationBedrockHeight(plan, x_m) : 0.0)),
            0.0, 1.0);
    }

    bool generationSubsurfaceBounds(
        const GenerationPlan &plan, const i64 min_x, const i64 min_y, const i64 max_x, const i64 max_y) noexcept {
        const auto &r = plan.recipe();
        if (min_x < r.m_boundary_fine or
            max_x > kWorldEdge - r.m_boundary_fine or
            min_y < r.m_boundary_fine or
            max_y > kWorldEdge - r.m_boundary_fine) {
            return false;
        }
        if (not r.m_open_surface) {
            return true;
        }
        for (i64 x = min_x; x < max_x; ++x) {
            const double xm = (static_cast<double>(x) + 0.5) * r.m_cell_size_m;
            if (static_cast<double>(min_y) * r.m_cell_size_m <= generationBedrockHeight(plan, xm) or
                static_cast<double>(max_y) * r.m_cell_size_m >= generationSurfaceHeight(plan, xm)) {
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // recipe validation
    // ------------------------------------------------------------------

    std::error_code validateGenerationHalo(
        const std::span<const GenerationBiome> biomes, const std::span<const std::string_view> paths,
        config::Diagnostics *const diagnostics) {
        using config::EConfigErrc;
        std::error_code first;
        const auto reject = [&](const std::string &path, const EConfigErrc code, const std::string_view message,
                                Array<std::string> related = {}) {
            const auto error = config::make_error_code(code);
            if (not first) {
                first = error;
            }
            if (diagnostics != nullptr) {
                diagnostics->add("definition", path, std::string{message}, error, {}, std::move(related));
            }
        };
        if (not paths.empty() and paths.size() != biomes.size()) {
            reject("biomes", EConfigErrc::schema_invalid, "Halo path count must match participant count");
            PPR_RETURN_ERROR_ON_FAIL(WorldPlan, first);
        }

        constexpr std::array<u32, 4> caps{10u, 25u, 10u, 25u};
        constexpr std::array<std::string_view, 4> names{
            "pore.passes", "relaxation_passes", "finish_passes", "erosion_passes"};
        std::array<u32, 4> maxima{};
        const auto counts = [](const GenerationBiome &biome) {
            return std::array{
                biome.m_pore_passes, biome.m_relaxation_passes, biome.m_finish_passes, biome.m_erosion_passes};
        };
        const auto path = [&](const std::size_t index, const std::size_t pass) {
            const auto prefix = paths.empty() ? "biomes." + std::to_string(index) : std::string{paths[index]};
            return prefix + "." + std::string{names[pass]};
        };

        for (std::size_t i = 0u; i < biomes.size(); ++i) {
            const auto values = counts(biomes[i]);
            for (std::size_t pass = 0u; pass < maxima.size(); ++pass) {
                maxima[pass] = std::max(maxima[pass], values[pass]);
                if (values[pass] > caps[pass]) {
                    reject(path(i, pass), EConfigErrc::out_of_range, "Sequential pass count exceeds its cap");
                }
            }
        }

        u64 total = 0u;
        for (const auto maximum: maxima) {
            total += maximum;
        }
        if (total > 24u) {
            Array<std::string> related;
            for (std::size_t i = 0u; i < biomes.size(); ++i) {
                const auto values = counts(biomes[i]);
                for (std::size_t pass = 0u; pass < maxima.size(); ++pass) {
                    if (maxima[pass] > 0u and values[pass] == maxima[pass]) {
                        related.push_back(path(i, pass));
                    }
                }
            }
            std::ranges::sort(related);
            related.erase(std::unique(related.begin(), related.end()), related.end());
            reject(
                "biomes", EConfigErrc::cross_field_invalid,
                "Sum of participant-wide sequential halo maxima exceeds 24 fine cells", std::move(related));
        }
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, first);
        return {};
    }

    // Legacy content v2: deferred migration — retain the full numeric world safety boundary.
    std::error_code validateGenerationRecipe(const GenerationRecipe &recipe) {
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateCounts(recipe));
        if (not positive(recipe.m_cell_size_m, 1.0) or
            not std::isfinite(1.0 / recipe.m_cell_size_m) or
            not positive(recipe.m_reference_thickness_m, 4096.0)) {
            PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
        }
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateMaterials(recipe));
        for (const auto &field: recipe.m_fields) {
            PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateNoiseFieldDescription(field.m_noise));
        }
        if (recipe.m_open_surface) {
            const auto *const bedrock = material(recipe, recipe.m_bedrock_material);
            const double extent = kWorldEdge * recipe.m_cell_size_m;
            if (recipe.m_surface_field >= recipe.m_fields.size() or
                recipe.m_bedrock_field >= recipe.m_fields.size() or
                bedrock == nullptr or
                not bedrock->m_terrain or
                bedrock->m_diggable or
                not interval(
                    recipe.m_bedrock_thickness_m - recipe.m_bedrock_variation_m,
                    recipe.m_bedrock_thickness_m + recipe.m_bedrock_variation_m, 15.0, 40.0) or
                not finiteRange(recipe.m_surface_amplitude_m, 0.0, 256.0) or
                not finiteRange(recipe.m_surface_elevation_m, 0.0, extent) or
                recipe.m_surface_elevation_m - recipe.m_surface_amplitude_m <= 40.0 or
                recipe.m_surface_elevation_m + recipe.m_surface_amplitude_m >= extent or
                (recipe.m_start_depth_m and not finiteRange(*recipe.m_start_depth_m, 0.0, extent))) {
                PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
            }
        }
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateBiomes(recipe));

        // Translate canonical diagnostics back to the established numeric recipe error contract.
        if (validateGenerationHalo(recipe.m_biomes, {})) {
            PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
        }

        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateFeatures(recipe));
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateLayout(recipe));
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateRules(recipe));
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateStart(recipe));
        return {};
    }

    Expected<GenerationPlan> compileGenerationPlan(GenerationRecipe recipe, const u64 seed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPlan, validateGenerationRecipe(recipe));

        for (auto &biome: recipe.m_biomes) {
            for (auto &band: biome.m_bands) {
                band.m_total_weight = 0.0;
                for (const auto &choice: band.m_choices) {
                    band.m_total_weight += choice.m_weight;
                }
            }
        }

        Array<NoiseField> fields;
        fields.reserve(recipe.m_fields.size());
        for (const auto &field: recipe.m_fields) {
            auto compiled = compileNoiseField(field.m_noise, generationStreamSeed(seed, field.m_key, 0u));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPlan, compiled);
            fields.push_back(std::move(*compiled));
        }

        std::ranges::sort(recipe.m_materials, {}, &GenerationMaterial::m_id);

        const std::size_t material_count = static_cast<std::size_t>(recipe.m_materials.back().m_id) + 1u;
        Array<GenerationMaterialLookup> material_lookup(material_count);
        material_lookup[0u] = {0x76616375756dull, 0.0};

        const auto semanticKey = [&](const GenerationMaterial &material) {
            if (material.m_key != 0u) {
                return material.m_key;
            }

            for (const auto &biome: recipe.m_biomes) {
                for (const auto &band: biome.m_bands) {
                    for (const auto &choice: band.m_choices) {
                        if (choice.m_material == material.m_id) {
                            return choice.m_key;
                        }
                    }
                }

                for (const auto &deposit: biome.m_deposits) {
                    if (deposit.m_material == material.m_id) {
                        return deposit.m_key;
                    }
                }
            }

            return u64{};
        };

        for (const auto &material: recipe.m_materials) {
            material_lookup[material.m_id] = {
                semanticKey(material), std::clamp(material.m_reference_density_kg_m3 / 5000.0, 0.0, 1.0)};
        }

        GenerationPlan plan{std::move(recipe), std::move(fields), std::move(material_lookup), seed};
        auto &resolved = plan.m_recipe;

        if (resolved.m_open_surface) {
            if (resolved.m_start_depth_m) {
                const double height = generationSurfaceHeight(
                                          plan, (resolved.m_start_x_fine + 0.5) * resolved.m_cell_size_m) -
                                      *resolved.m_start_depth_m;
                if (height < 0.0 or height >= kWorldEdge * resolved.m_cell_size_m) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }
                resolved.m_start_y_fine = static_cast<u32>(std::floor(height / resolved.m_cell_size_m));
            }
            const auto &bounds = resolved.m_features[resolved.m_start_feature].m_bounds;
            if (not generationSubsurfaceBounds(
                    plan, static_cast<i64>(resolved.m_start_x_fine) + bounds.m_min_x,
                    static_cast<i64>(resolved.m_start_y_fine) + bounds.m_min_y,
                    static_cast<i64>(resolved.m_start_x_fine) + bounds.m_max_x,
                    static_cast<i64>(resolved.m_start_y_fine) + bounds.m_max_y)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPlan, std::errc::invalid_argument);
            }
        }
        return plan;
    }
}
