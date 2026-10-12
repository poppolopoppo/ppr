module;
#include "pP/Macros.h"

module engine.sim;
import :worldgen.plan;
import :chunk_grid;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldPlan, info, none);

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
                    entry.m_matter > EGenerationMatter::gas or
                    (entry.m_terrain and entry.m_matter != EGenerationMatter::solid) or
                    not positive(entry.m_reference_density_kg_m3, (std::numeric_limits<double>::max)())) {
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
                    ((biome.m_lining_material == 0u) != (biome.m_lining_thickness_fine == 0u)) or
                    (biome.m_lining_material != 0u and not terrain(recipe, biome.m_lining_material)) or
                    not ordered(biome.m_bands) or
                    not ordered(biome.m_deposits)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                }
                double previous_depth = 0.0;
                for (const auto &band: biome.m_bands) {
                    if (band.m_field >= recipe.m_fields.size() or
                        not interval(band.m_depth_min, band.m_depth_max, 0.0, 1.0) or
                        band.m_depth_min != previous_depth or
                        band.m_depth_min == band.m_depth_max or
                        band.m_choices.empty() or
                        band.m_choices.size() > kMaxGenerationMaterials or
                        not uniqueKeys(band.m_choices) or
                        not validWeights(band.m_choices)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                    for (const auto &choice: band.m_choices) {
                        if (not terrain(recipe, choice.m_material)) {
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
                        not interval(deposit.m_threshold_min, deposit.m_threshold_max, -1.0, 1.0)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, std::errc::invalid_argument);
                    }
                }
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
                               (value.m_carve or (terrain(recipe, value.m_material) and terrain(recipe, value.m_host)));
                    } else if constexpr (std::is_same_v<T, GenerationConnection>) {
                        return rule.m_phase == EGenerationPhase::carve and
                               positive(value.m_radius_m, 256.0) and
                               value.m_extra_links <= 1024u;
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
    GenerationPlan::GenerationPlan(GenerationRecipe recipe, Array<NoiseField> fields, const u64 seed) noexcept
        : m_recipe(std::move(recipe)), m_fields(std::move(fields)), m_seed(seed) {
    }
    const GenerationRecipe &GenerationPlan::recipe() const noexcept {
        return m_recipe;
    }
    std::span<const NoiseField> GenerationPlan::fields() const noexcept {
        return m_fields;
    }
    u64 GenerationPlan::seed() const noexcept {
        return m_seed;
    }

    u64 GenerationPlan::retainedPayloadBytes() const noexcept {
        u64 bytes = m_fields.capacity() * sizeof(NoiseField) +
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
        PPR_RETURN_ERROR_ON_FAIL(WorldPlan, validateBiomes(recipe));
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
        return GenerationPlan{std::move(recipe), std::move(fields), seed};
    }
}
