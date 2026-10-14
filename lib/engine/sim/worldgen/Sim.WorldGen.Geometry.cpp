module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.geometry;
import :chunk_grid;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldGeometry, info, none);

    // ------------------------------------------------------------------
    // geometry helpers
    // ------------------------------------------------------------------

    namespace {
        [[nodiscard]] std::error_code canceled(const SharedContext &context) noexcept {
            return context != nullptr ? context->error() : std::error_code{};
        }

        struct Rect {
            i64 m_left{}, m_bottom{}, m_right{}, m_top{};
        };

        [[nodiscard]] Rect reservation(const GenerationRecipe &recipe, const GenerationInstance &instance) noexcept {
            const auto &bounds = recipe.m_features[instance.m_feature].m_bounds;
            return {
                instance.m_origin_x + static_cast<i64>(bounds.m_min_x),
                instance.m_origin_y + static_cast<i64>(bounds.m_min_y),
                instance.m_origin_x + static_cast<i64>(bounds.m_max_x),
                instance.m_origin_y + static_cast<i64>(bounds.m_max_y)};
        }

        [[nodiscard]] double spacing(const GenerationRecipe &recipe, const GenerationInstance &instance) noexcept {
            return instance.m_rule == kGenerationStartRule
                       ? 0.0
                       : std::get<GenerationPlacement>(recipe.m_rules[instance.m_rule].m_parameters).m_spacing_m;
        }

        [[nodiscard]] bool fits(
            const GenerationPlan &plan, const GenerationGeometry &geometry,
            const GenerationInstance &candidate) noexcept {
            const auto &recipe = plan.recipe();
            const Rect bounds = reservation(recipe, candidate);
            if (not generationSubsurfaceBounds(plan, bounds.m_left, bounds.m_bottom, bounds.m_right, bounds.m_top)) {
                return false;
            }

            const i64 boundary = recipe.m_boundary_fine;
            if (bounds.m_left < boundary or
                bounds.m_bottom < boundary or
                bounds.m_right > kWorldEdge - boundary or
                bounds.m_top > kWorldEdge - boundary) {
                return false;
            }

            const double centre_x = 0.5 * static_cast<double>(bounds.m_left + bounds.m_right);
            const double centre_y = 0.5 * static_cast<double>(bounds.m_bottom + bounds.m_top);
            for (const auto &instance: geometry.m_instances) {
                const Rect other = reservation(recipe, instance);
                if (bounds.m_left < other.m_right and
                    bounds.m_right > other.m_left and
                    bounds.m_bottom < other.m_top and
                    bounds.m_top > other.m_bottom) {
                    return false;
                }
                const double dx = (centre_x - 0.5 * static_cast<double>(other.m_left + other.m_right)) /
                                  (1.0 / recipe.m_cell_size_m);
                const double dy = (centre_y - 0.5 * static_cast<double>(other.m_bottom + other.m_top)) /
                                  (1.0 / recipe.m_cell_size_m);
                if (std::hypot(dx, dy) < std::max(spacing(recipe, candidate), spacing(recipe, instance))) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] Expected<bool> place(
            const GenerationPlan &plan, const BiomeLayout &layout, GenerationGeometry &geometry, const u32 rule_index,
            const u32 ordinal, const SharedContext &context) {
            const auto &recipe = plan.recipe();
            const auto &rule = recipe.m_rules[rule_index];
            const auto &placement = std::get<GenerationPlacement>(rule.m_parameters);
            const auto &bounds = recipe.m_features[placement.m_feature].m_bounds;
            const i64 min_x = static_cast<i64>(recipe.m_boundary_fine) - bounds.m_min_x;
            const i64 min_y = static_cast<i64>(recipe.m_boundary_fine) - bounds.m_min_y;
            const i64 max_x = static_cast<i64>(kWorldEdge - recipe.m_boundary_fine) - bounds.m_max_x;
            const i64 max_y = static_cast<i64>(kWorldEdge - recipe.m_boundary_fine) - bounds.m_max_y;
            if (min_x > max_x or min_y > max_y) {
                return false;
            }

            GenerationRandom random{generationStreamSeed(plan.seed(), rule.m_key, ordinal)};
            for (u32 attempt = 0u; attempt < placement.m_attempts; ++attempt) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, canceled(context));
                geometry.m_worst_placement_attempts = std::max(geometry.m_worst_placement_attempts, attempt + 1u);
                GenerationInstance candidate{
                    placement.m_feature, rule_index, ordinal,
                    static_cast<i32>(min_x + static_cast<i64>(random.bounded(static_cast<u64>(max_x - min_x + 1)))),
                    static_cast<i32>(min_y + static_cast<i64>(random.bounded(static_cast<u64>(max_y - min_y + 1)))),
                    placement.m_reflect and random.bounded(2u) != 0u};
                const Rect rect = reservation(recipe, candidate);
                const double x = 0.5 * static_cast<double>(rect.m_left + rect.m_right) / (1.0 / recipe.m_cell_size_m);
                const double y = 0.5 * static_cast<double>(rect.m_bottom + rect.m_top) / (1.0 / recipe.m_cell_size_m);
                const double depth = generationDepth(plan, x, y);
                if (depth < placement.m_depth_min or
                    depth > placement.m_depth_max or
                    generationBiomeAt(plan, layout, x, y) != placement.m_biome or
                    not fits(plan, geometry, candidate)) {
                    continue;
                }
                geometry.m_instances.push_back(candidate);
                return true;
            }
            return false;
        }

        [[nodiscard]] std::error_code features(
            const GenerationPlan &plan, const BiomeLayout &layout, GenerationGeometry &geometry,
            const SharedContext &context, u64 *const known_scratch_peak) {
            const auto &recipe = plan.recipe();
            geometry.m_instances.push_back(
                {recipe.m_start_feature, kGenerationStartRule, 0u, static_cast<i32>(recipe.m_start_x_fine),
                    static_cast<i32>(recipe.m_start_y_fine), false});
            Array<u32> placements;
            PPR_DEFER {
                if (known_scratch_peak != nullptr) {
                    *known_scratch_peak = std::max(*known_scratch_peak, placements.capacity() * sizeof(u32));
                }
            };
            for (u32 r = 0u; r < recipe.m_rules.size(); ++r) {
                if (std::holds_alternative<GenerationPlacement>(recipe.m_rules[r].m_parameters)) {
                    placements.push_back(r);
                }
            }

            std::ranges::sort(placements, [&](const u32 a, const u32 b) {
                const auto &pa = std::get<GenerationPlacement>(recipe.m_rules[a].m_parameters);
                const auto &pb = std::get<GenerationPlacement>(recipe.m_rules[b].m_parameters);
                if (pa.m_priority != pb.m_priority) {
                    return pa.m_priority > pb.m_priority;
                }
                return std::tie(recipe.m_rules[a].m_order, recipe.m_rules[a].m_key) <
                       std::tie(recipe.m_rules[b].m_order, recipe.m_rules[b].m_key);
            });

            // Reserve every required instance before optional instances compete
            // for remaining space; priority orders conflicts within each pass.
            for (const bool required: {true, false}) {
                for (const u32 r: placements) {
                    const auto &placement = std::get<GenerationPlacement>(recipe.m_rules[r].m_parameters);
                    const u32 begin = required ? 0u : placement.m_min_count;
                    const u32 end = required ? placement.m_min_count : placement.m_max_count;
                    for (u32 ordinal = begin; ordinal < end; ++ordinal) {
                        auto result = place(plan, layout, geometry, r, ordinal, context);
                        PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, result);
                        if (*result) {
                            continue;
                        }
                        if (required) {
                            PPR_RETURN_ERROR_ON_FAIL(
                                WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                        }
                        geometry.m_omitted_instances.push_back(r);
                    }
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code addSegment(GenerationGeometry &geometry, const GenerationSegment segment) {
            if (geometry.m_segments.size() >= kMaxGenerationSegments) {
                PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, make_error_code(EGenerationErrc::segment_limit));
            }
            geometry.m_segments.push_back(segment);
            return {};
        }

        struct BlobOriginRegion {
            double m_left{}, m_bottom{}, m_right{}, m_top{}, m_cumulative_area{};
        };

        [[nodiscard]] Expected<Array<BlobOriginRegion>> blobOriginRegions(
            const GenerationPlan &plan, const BiomeLayout &layout, const u32 biome, const SharedContext &context) {
            const auto &recipe = plan.recipe();
            const double extent = kWorldEdge * recipe.m_cell_size_m;
            const double boundary = recipe.m_boundary_fine * recipe.m_cell_size_m;
            Array<BlobOriginRegion> regions;
            for (const auto &site: layout.sites()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, canceled(context));
                if (site.m_biome != biome) {
                    continue;
                }
                double nearest_foreign = 2.0 * extent;
                for (const auto &other: layout.sites()) {
                    if (other.m_biome != biome) {
                        nearest_foreign = std::min(
                            nearest_foreign, std::hypot(site.m_x_m - other.m_x_m, site.m_y_m - other.m_y_m));
                    }
                }
                // Proposals follow the macro province scale rather than the
                // deposit radius. Include the bounded displacement on both axes;
                // the final biome query remains authoritative after domain warp.
                const double radius = 0.5 * nearest_foreign + std::numbers::sqrt2 * recipe.m_boundary_warp_amplitude_m;
                regions.push_back(
                    {std::max(boundary, site.m_x_m - radius), std::max(boundary, site.m_y_m - radius),
                        std::min(extent - boundary, site.m_x_m + radius),
                        std::min(extent - boundary, site.m_y_m + radius), 0.0});
            }
            return regions;
        }

        [[nodiscard]] std::error_code blobs(
            const GenerationPlan &plan, const BiomeLayout &layout, GenerationGeometry &geometry,
            const SharedContext &context, u64 *const known_scratch_peak) {
            const auto &recipe = plan.recipe();
            const double extent = kWorldEdge * recipe.m_cell_size_m;
            const double boundary = recipe.m_boundary_fine * recipe.m_cell_size_m;
            for (u32 b = 0u; b < recipe.m_biomes.size(); ++b) {
                if (layout.sampleBiome() ? b != *layout.sampleBiome()
                                         : std::ranges::none_of(recipe.m_constraints, [b](const auto &entry) {
                                               return entry.m_biome == b;
                                           })) {
                    continue;
                }
                if (std::ranges::none_of(recipe.m_biomes[b].m_deposits, [](const auto &deposit) {
                        return deposit.m_blob_count != 0u;
                    })) {
                    continue;
                }
                PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                const auto province_regions = blobOriginRegions(plan, layout, b, context);
                PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, province_regions);
                for (u32 d = 0u; d < recipe.m_biomes[b].m_deposits.size(); ++d) {
                    const auto &deposit = recipe.m_biomes[b].m_deposits[d];
                    if (deposit.m_blob_count == 0u) {
                        continue;
                    }
                    Array<BlobOriginRegion> regions;
                    double area = 0.0;
                    const double bottom = recipe.m_open_surface
                                              ? boundary
                                              : std::max(boundary, extent * (1.0 - deposit.m_depth_max));
                    const double top = recipe.m_open_surface
                                           ? extent - boundary
                                           : std::min(extent - boundary, extent * (1.0 - deposit.m_depth_min));
                    for (auto region: *province_regions) {
                        region.m_bottom = std::max(region.m_bottom, bottom);
                        region.m_top = std::min(region.m_top, top);
                        if (region.m_left < region.m_right and region.m_bottom < region.m_top) {
                            area += (region.m_right - region.m_left) * (region.m_top - region.m_bottom);
                            region.m_cumulative_area = area;
                            regions.push_back(region);
                        }
                    }
                    if (known_scratch_peak != nullptr) {
                        *known_scratch_peak = std::max(
                            *known_scratch_peak,
                            (province_regions->capacity() + regions.capacity()) * sizeof(BlobOriginRegion));
                    }
                    if (regions.empty()) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                    }
                    for (u32 ordinal = 0u; ordinal < deposit.m_blob_count; ++ordinal) {
                        GenerationRandom random{generationStreamSeed(plan.seed(), deposit.m_key, ordinal)};
                        double x{}, y{};
                        bool found = false;
                        for (u32 attempt = 0u; attempt < kGenerationBlobOriginAttempts; ++attempt) {
                            PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                            const double draw = random.unit() * area;
                            const auto region = std::ranges::upper_bound(
                                regions, draw, {}, &BlobOriginRegion::m_cumulative_area);
                            const auto &proposal = region == regions.end() ? regions.back() : *region;
                            x = proposal.m_left + (proposal.m_right - proposal.m_left) * random.unit();
                            y = proposal.m_bottom + (proposal.m_top - proposal.m_bottom) * random.unit();
                            const double depth = generationDepth(plan, x, y);
                            if ((not recipe.m_open_surface or
                                    (y > generationBedrockHeight(plan, x) and y < generationSurfaceHeight(plan, x))) and
                                depth >= deposit.m_depth_min and
                                depth <= deposit.m_depth_max and
                                generationBiomeAt(plan, layout, x, y) == b) {
                                found = true;
                                break;
                            }
                        }
                        if (not found) {
                            PPR_RETURN_ERROR_ON_FAIL(
                                WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                        }
                        geometry.m_blobs.push_back(
                            {b, d, ordinal, x, y,
                                deposit.m_radius_min_m +
                                    (deposit.m_radius_max_m - deposit.m_radius_min_m) * random.unit(),
                                deposit.m_stretch_min + (deposit.m_stretch_max - deposit.m_stretch_min) * random.unit(),
                                2.0 *
                                    std::numbers::pi *
                                    (deposit.m_orientation_min_degrees / 360.0 +
                                        (deposit.m_orientation_max_degrees - deposit.m_orientation_min_degrees) /
                                            360.0 *
                                            random.unit()),
                                random.unit() * 1024.0, random.unit() * 1024.0});
                    }
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code smoothWorm(
            const GenerationRecipe &recipe, const GenerationWorm &worm, const u32 rule, GenerationRandom &random,
            GenerationGeometry &geometry, double x, double y, const SharedContext &context) {
            const double extent = kWorldEdge * recipe.m_cell_size_m;
            const double boundary = recipe.m_boundary_fine * recipe.m_cell_size_m;
            double remaining = worm.m_length_min_m + (worm.m_length_max_m - worm.m_length_min_m) * random.unit();
            const double initial_radius = worm.m_radius_min_m +
                                          (worm.m_radius_max_m - worm.m_radius_min_m) * random.unit();
            double radius = initial_radius, target_radius = initial_radius;
            double heading = random.unit() * 2.0 * std::numbers::pi, angular_velocity = 0.0, target_turn = 0.0;
            double chamber_left = 0.0, chamber_length = 1.0;
            for (u32 step = 0u; remaining > 0.0; ++step) {
                PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                if (random.unit() < worm.m_turn_probability) {
                    target_turn = (random.unit() - 0.5) * 0.7;
                }
                angular_velocity = worm.m_turn_smoothing * angular_velocity +
                                   (1.0 - worm.m_turn_smoothing) * target_turn;
                heading += angular_velocity;
                if (step % 8u == 0u) {
                    target_radius = initial_radius * (1.0 - worm.m_radius_variation) +
                                    worm.m_radius_variation *
                                        (worm.m_radius_min_m +
                                            (worm.m_radius_max_m - worm.m_radius_min_m) * random.unit());
                }
                radius += 0.15 * (target_radius - radius);
                if (chamber_left <= 0.0 and random.unit() < worm.m_chamber_probability) {
                    chamber_length = 6.0 + random.unit() * 10.0;
                    chamber_left = chamber_length;
                }
                const double chamber = chamber_left > 0.0
                                           ? std::sin(std::numbers::pi * (1.0 - chamber_left / chamber_length))
                                           : 0.0;
                const double length = std::min(1.0, remaining);
                const double next_x = std::clamp(x + std::cos(heading) * length, boundary, extent - boundary);
                const double next_y = std::clamp(y + std::sin(heading) * length, boundary, extent - boundary);
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldGeometry, addSegment(
                                       geometry, {rule, x, y, next_x, next_y,
                                                     radius * (1.0 + (worm.m_chamber_scale - 1.0) * chamber)}));
                x = next_x;
                y = next_y;
                remaining -= length;
                chamber_left -= length;
            }
            return {};
        }

        [[nodiscard]] std::error_code worms(
            const GenerationPlan &plan, const BiomeLayout &layout, GenerationGeometry &geometry,
            const SharedContext &context) {
            const auto &recipe = plan.recipe();
            constexpr double diagonal = 0.7071067811865475244;
            constexpr std::array<std::array<double, 2u>, 8u> headings{
                {{1.0, 0.0}, {diagonal, diagonal}, {0.0, 1.0}, {-diagonal, diagonal}, {-1.0, 0.0},
                    {-diagonal, -diagonal}, {0.0, -1.0}, {diagonal, -diagonal}}};
            const double extent = kWorldEdge / (1.0 / recipe.m_cell_size_m);
            const double boundary = recipe.m_boundary_fine / (1.0 / recipe.m_cell_size_m);
            for (u32 r = 0u; r < recipe.m_rules.size(); ++r) {
                const auto &rule = recipe.m_rules[r];
                const auto *const worm = std::get_if<GenerationWorm>(&rule.m_parameters);
                if (worm == nullptr) {
                    continue;
                }
                if (layout.sampleBiome() and worm->m_biome != *layout.sampleBiome()) {
                    continue;
                }
                // One-metre maximum steps; the final step retains authored
                // sub-metre lengths rather than rounding them onto the grid.
                const double segment_count = std::ceil(worm->m_length_max_m);
                if (segment_count * worm->m_count > kMaxGenerationSegments - geometry.m_segments.size()) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, make_error_code(EGenerationErrc::segment_limit));
                }
                for (u32 ordinal = 0u; ordinal < worm->m_count; ++ordinal) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                    GenerationRandom random{generationStreamSeed(plan.seed(), rule.m_key, ordinal)};
                    double x = 0.0, y = 0.0;
                    bool found = false;
                    for (u32 attempt = 0u; attempt < kGenerationWormOriginAttempts; ++attempt) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                        x = boundary + (extent - 2.0 * boundary) * random.unit();
                        y = boundary + (extent - 2.0 * boundary) * random.unit();
                        const double depth = generationDepth(plan, x, y);
                        if ((not recipe.m_open_surface or
                                (y > generationBedrockHeight(plan, x) and y < generationSurfaceHeight(plan, x))) and
                            depth >= worm->m_depth_min and
                            depth <= worm->m_depth_max and
                            generationBiomeAt(plan, layout, x, y) == worm->m_biome) {
                            found = true;
                            break;
                        }
                    }
                    if (not found) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                    }
                    if (worm->m_turn_smoothing > 0.0) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            WorldGeometry, smoothWorm(recipe, *worm, r, random, geometry, x, y, context));
                        continue;
                    }
                    double remaining = worm->m_length_min_m +
                                       (worm->m_length_max_m - worm->m_length_min_m) * random.unit();
                    const double radius = worm->m_radius_min_m +
                                          (worm->m_radius_max_m - worm->m_radius_min_m) * random.unit();
                    u32 heading = static_cast<u32>(random.bounded(headings.size()));
                    const auto steps = static_cast<u32>(std::ceil(remaining));
                    for (u32 step = 0u; step < steps; ++step) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                        if (random.unit() < worm->m_turn_probability) {
                            heading = (heading + (random.bounded(2u) == 0u ? 1u : 7u)) % 8u;
                        }
                        const double length = std::min(1.0, remaining);
                        const double next_x = std::clamp(
                            x + headings[heading][0u] * length, boundary, extent - boundary);
                        const double next_y = std::clamp(
                            y + headings[heading][1u] * length, boundary, extent - boundary);
                        PPR_RETURN_ERROR_ON_FAIL(
                            WorldGeometry, addSegment(geometry, {r, x, y, next_x, next_y, radius}));
                        x = next_x;
                        y = next_y;
                        remaining -= length;
                    }
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code connections(
            const GenerationPlan &plan, const BiomeLayout &layout, GenerationGeometry &geometry,
            const SharedContext &context, u64 *const known_scratch_peak) {
            const auto &recipe = plan.recipe();
            if (std::ranges::none_of(recipe.m_rules, [](const auto &rule) {
                    return std::holds_alternative<GenerationConnection>(rule.m_parameters);
                })) {
                return {};
            }
            struct Anchor {
                double m_x{}, m_y{};
            };
            Array<Anchor> anchors;
            PPR_DEFER {
                if (known_scratch_peak != nullptr) {
                    *known_scratch_peak = std::max(*known_scratch_peak, anchors.capacity() * sizeof(Anchor));
                }
            };
            for (const auto &instance: geometry.m_instances) {
                const auto &feature = recipe.m_features[instance.m_feature];
                // Sealed-container protection takes precedence over room markers.
                if (instance.m_rule == kGenerationStartRule or
                    std::ranges::any_of(feature.m_markers, [](const auto &marker) {
                        return marker.m_role == EGenerationMarker::sealed_pocket;
                    })) {
                    continue;
                }
                for (const auto &marker: feature.m_markers) {
                    if (marker.m_role != EGenerationMarker::traversal_room) {
                        continue;
                    }
                    if (anchors.size() >= 1024u) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, make_error_code(EGenerationErrc::room_anchor_limit));
                    }
                    const double local_x = instance.m_reflected ? static_cast<double>(feature.m_bounds.m_min_x) +
                                                                      feature.m_bounds.m_max_x -
                                                                      marker.m_x -
                                                                      0.5
                                                                : marker.m_x + 0.5;
                    anchors.push_back(
                        {(instance.m_origin_x + local_x) / (1.0 / recipe.m_cell_size_m),
                            (instance.m_origin_y + marker.m_y + 0.5) / (1.0 / recipe.m_cell_size_m)});
                }
            }
            for (u32 r = 0u; r < recipe.m_rules.size(); ++r) {
                const auto *const connection = std::get_if<GenerationConnection>(&recipe.m_rules[r].m_parameters);
                if (connection == nullptr) {
                    continue;
                }
                const double extent = kWorldEdge * recipe.m_cell_size_m;
                const double clearance = connection->m_radius_m * 1.25 + std::numbers::sqrt2 * recipe.m_cell_size_m;
                Array<double> surface(kWorldEdge), bedrock(kWorldEdge);
                for (u32 x = 0u; x < kWorldEdge; ++x) {
                    surface[x] = generationSurfaceHeight(plan, (x + 0.5) * recipe.m_cell_size_m);
                    bedrock[x] = generationBedrockHeight(plan, (x + 0.5) * recipe.m_cell_size_m);
                }
                const auto clearPoint = [&](const double x, const double y) {
                    const double boundary = recipe.m_boundary_fine * recipe.m_cell_size_m;
                    if (x - clearance <= boundary or
                        x + clearance >= extent - boundary or
                        y - clearance <= boundary or
                        y + clearance >= extent - boundary) {
                        return false;
                    }
                    if (recipe.m_open_surface) {
                        const u32 left = static_cast<u32>(std::floor((x - clearance) / recipe.m_cell_size_m));
                        const u32 right = static_cast<u32>(std::ceil((x + clearance) / recipe.m_cell_size_m));
                        for (u32 column = left; column <= right; ++column) {
                            if (y - clearance <= bedrock[column] or y + clearance >= surface[column]) {
                                return false;
                            }
                        }
                    }
                    return true;
                };
                struct Obstacle {
                    double left{}, bottom{}, right{}, top{};
                };
                Array<Obstacle> obstacles;
                for (const auto &instance: geometry.m_instances) {
                    const auto &feature = recipe.m_features[instance.m_feature];
                    const bool sealed = std::ranges::any_of(
                        feature.m_markers, [](const auto &m) { return m.m_role == EGenerationMarker::sealed_pocket; });
                    const bool room = std::ranges::any_of(
                        feature.m_markers, [](const auto &m) { return m.m_role == EGenerationMarker::traversal_room; });
                    if (instance.m_rule != kGenerationStartRule and room and not sealed) {
                        continue;
                    }
                    const auto bounds = reservation(recipe, instance);
                    obstacles.push_back(
                        {static_cast<double>(bounds.m_left) * recipe.m_cell_size_m - clearance,
                            static_cast<double>(bounds.m_bottom) * recipe.m_cell_size_m - clearance,
                            static_cast<double>(bounds.m_right) * recipe.m_cell_size_m + clearance,
                            static_cast<double>(bounds.m_top) * recipe.m_cell_size_m + clearance});
                }
                const auto clearLine = [&](const Anchor a, const Anchor b) {
                    if (not clearPoint(a.m_x, a.m_y) or not clearPoint(b.m_x, b.m_y)) {
                        return false;
                    }
                    for (const auto &box: obstacles) {
                        double low = 0.0, high = 1.0;
                        const auto slab = [&](const double origin, const double delta, const double minimum,
                                              const double maximum) {
                            if (std::abs(delta) < 1.0e-12) {
                                return origin >= minimum and origin <= maximum;
                            }
                            const double t0 = (minimum - origin) / delta, t1 = (maximum - origin) / delta;
                            low = std::max(low, std::min(t0, t1));
                            high = std::min(high, std::max(t0, t1));
                            return low <= high;
                        };
                        if (slab(a.m_x, b.m_x - a.m_x, box.left, box.right) and
                            slab(a.m_y, b.m_y - a.m_y, box.bottom, box.top)) {
                            return false;
                        }
                    }
                    const u32 steps = static_cast<u32>(
                        std::ceil(std::hypot(b.m_x - a.m_x, b.m_y - a.m_y) / recipe.m_cell_size_m));
                    for (u32 step = 1u; step < steps; ++step) {
                        const double t = static_cast<double>(step) / steps;
                        if (not clearPoint(a.m_x + t * (b.m_x - a.m_x), a.m_y + t * (b.m_y - a.m_y))) {
                            return false;
                        }
                    }
                    return true;
                };
                Array<std::pair<u64, Anchor>> natural;
                for (u32 site = 0u; site < layout.sites().size(); ++site) {
                    if (site == 0u) {
                        continue;
                    }
                    const auto &point = layout.sites()[site];
                    const Anchor anchor{point.m_x_m, point.m_y_m};
                    if (clearLine(anchor, anchor)) {
                        natural.push_back(
                            {generationStreamSeed(plan.seed(), recipe.m_rules[r].m_key ^ 0x616e63686f7273ull, site),
                                anchor});
                    }
                }
                std::ranges::sort(natural, {}, &std::pair<u64, Anchor>::first);
                if (natural.size() < connection->m_natural_anchors or
                    anchors.size() + connection->m_natural_anchors > 1024u) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                }
                for (u32 i = 0u; i < connection->m_natural_anchors; ++i) {
                    anchors.push_back(natural[i].second);
                }
                if (anchors.size() < 2u) {
                    continue;
                }
                GenerationRandom route_random{
                    generationStreamSeed(plan.seed(), recipe.m_rules[r].m_key, 0x726f757465ull)};
                const auto route = [&](const Anchor a, const Anchor b) -> std::error_code {
                    if (connection->m_natural_anchors == 0u and
                        connection->m_roughness_m == 0.0 and
                        connection->m_wells == 0u and
                        connection->m_branches == 0u) {
                        return addSegment(geometry, {r, a.m_x, a.m_y, b.m_x, b.m_y, connection->m_radius_m});
                    }
                    Array<Anchor> path;
                    if (clearLine(a, b)) {
                        path = {a, b};
                    } else {
                        // Bounded eight-neighbor A* on an eight-metre planning grid.
                        // Every admitted edge has continuous radius-aware clearance.
                        const double step = 8.0;
                        const u32 side = static_cast<u32>(std::ceil(extent / step));
                        const u32 count = side * side;
                        const auto point = [&](const u32 id) {
                            return Anchor{(id % side + 0.5) * step, (id / side + 0.5) * step};
                        };
                        const auto endpoint = [&](const Anchor at) -> std::optional<u32> {
                            const i32 cx = static_cast<i32>(at.m_x / step), cy = static_cast<i32>(at.m_y / step);
                            for (i32 ring = 0; ring <= 4; ++ring) {
                                for (i32 dy = -ring; dy <= ring; ++dy) {
                                    for (i32 dx = -ring; dx <= ring; ++dx) {
                                        if (std::max(std::abs(dx), std::abs(dy)) != ring) {
                                            continue;
                                        }
                                        const i32 x = cx + dx, y = cy + dy;
                                        if (x < 0 or
                                            y < 0 or
                                            x >= static_cast<i32>(side) or
                                            y >= static_cast<i32>(side)) {
                                            continue;
                                        }
                                        const u32 id = static_cast<u32>(y) * side + static_cast<u32>(x);
                                        if (clearLine(at, point(id))) {
                                            return id;
                                        }
                                    }
                                }
                            }
                            return {};
                        };
                        const auto first = endpoint(a), last = endpoint(b);
                        if (not first or not last) {
                            PPR_RETURN_ERROR_ON_FAIL(
                                WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                        }
                        Array<double> costs(count, (std::numeric_limits<double>::max)());
                        Array<i32> previous(count, -1);
                        Array<u8> closed(count, 0u);
                        using QueueItem = std::pair<double, u32>;
                        std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
                        costs[*first] = 0.0;
                        queue.push({0.0, *first});
                        u32 expanded = 0u;
                        while (not queue.empty() and expanded < 65536u) {
                            PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                            const u32 current = queue.top().second;
                            queue.pop();
                            if (closed[current] != 0u) {
                                continue;
                            }
                            closed[current] = 1u;
                            ++expanded;
                            if (current == *last) {
                                break;
                            }
                            const Anchor origin = point(current);
                            for (i32 dy = -1; dy <= 1; ++dy) {
                                for (i32 dx = -1; dx <= 1; ++dx) {
                                    if (dx == 0 and dy == 0) {
                                        continue;
                                    }
                                    const i32 x = static_cast<i32>(current % side) + dx,
                                              y = static_cast<i32>(current / side) + dy;
                                    if (x < 0 or y < 0 or x >= static_cast<i32>(side) or y >= static_cast<i32>(side)) {
                                        continue;
                                    }
                                    const u32 next = static_cast<u32>(y) * side + static_cast<u32>(x);
                                    if (closed[next] != 0u) {
                                        continue;
                                    }
                                    const Anchor target = point(next);
                                    const double cost = costs[current] +
                                                        std::hypot(target.m_x - origin.m_x, target.m_y - origin.m_y);
                                    if (cost >= costs[next] or not clearLine(origin, target)) {
                                        continue;
                                    }
                                    costs[next] = cost;
                                    previous[next] = static_cast<i32>(current);
                                    if (queue.size() >= 262144u) {
                                        PPR_RETURN_ERROR_ON_FAIL(
                                            WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                                    }
                                    queue.push({cost + std::hypot(target.m_x - b.m_x, target.m_y - b.m_y), next});
                                }
                            }
                        }
                        if (closed[*last] == 0u) {
                            PPR_RETURN_ERROR_ON_FAIL(
                                WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                        }
                        path.push_back(b);
                        for (i32 id = static_cast<i32>(*last); id >= 0; id = previous[static_cast<u32>(id)]) {
                            path.push_back(point(static_cast<u32>(id)));
                        }
                        path.push_back(a);
                        std::ranges::reverse(path);
                        if (known_scratch_peak != nullptr) {
                            *known_scratch_peak = std::max(
                                *known_scratch_peak,
                                static_cast<u64>(count) * (sizeof(double) + sizeof(i32) + sizeof(u8)) +
                                    262144u * sizeof(QueueItem) +
                                    path.capacity() * sizeof(Anchor) +
                                    anchors.capacity() * sizeof(Anchor) +
                                    (surface.capacity() + bedrock.capacity()) * sizeof(double));
                        }
                    }
                    for (u32 leg = 1u; leg < path.size(); ++leg) {
                        const Anchor from = path[leg - 1u], to = path[leg];
                        const double distance = std::hypot(to.m_x - from.m_x, to.m_y - from.m_y);
                        const u32 steps = std::max(1u, static_cast<u32>(std::ceil(distance / 12.0)));
                        Anchor previous = from;
                        for (u32 i = 1u; i <= steps; ++i) {
                            const double t = static_cast<double>(i) / steps;
                            const Anchor target = i == steps ? to
                                                             : Anchor{
                                                                   from.m_x + t * (to.m_x - from.m_x),
                                                                   from.m_y + t * (to.m_y - from.m_y)};
                            const double bend = (route_random.unit() - 0.5) * 2.0 * connection->m_roughness_m;
                            const Anchor middle{
                                0.5 * (previous.m_x + target.m_x) -
                                    (distance > 0.0 ? (to.m_y - from.m_y) / distance * bend : 0.0),
                                0.5 * (previous.m_y + target.m_y) +
                                    (distance > 0.0 ? (to.m_x - from.m_x) / distance * bend : 0.0)};
                            const double radius = connection->m_radius_m * (1.0 + 0.25 * route_random.unit());
                            if (connection->m_roughness_m > 0.0 and
                                clearLine(previous, middle) and
                                clearLine(middle, target)) {
                                PPR_RETURN_ERROR_ON_FAIL(
                                    WorldGeometry,
                                    addSegment(
                                        geometry, {r, previous.m_x, previous.m_y, middle.m_x, middle.m_y, radius}));
                                PPR_RETURN_ERROR_ON_FAIL(
                                    WorldGeometry,
                                    addSegment(geometry, {r, middle.m_x, middle.m_y, target.m_x, target.m_y, radius}));
                            } else {
                                PPR_RETURN_ERROR_ON_FAIL(
                                    WorldGeometry,
                                    addSegment(
                                        geometry, {r, previous.m_x, previous.m_y, target.m_x, target.m_y, radius}));
                            }
                            previous = target;
                        }
                    }
                    return {};
                };
                struct Edge {
                    u32 m_a{}, m_b{};
                    double m_distance{};
                };
                Array<Edge> edges;
                for (u32 a = 0u; a < anchors.size(); ++a) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                    for (u32 b = a + 1u; b < anchors.size(); ++b) {
                        const double dx = anchors[a].m_x - anchors[b].m_x, dy = anchors[a].m_y - anchors[b].m_y;
                        edges.push_back({a, b, dx * dx + dy * dy});
                    }
                }
                std::ranges::sort(edges, [](const Edge &a, const Edge &b) {
                    return std::tie(a.m_distance, a.m_a, a.m_b) < std::tie(b.m_distance, b.m_a, b.m_b);
                });
                Array<u32> parents(anchors.size());
                std::iota(parents.begin(), parents.end(), 0u);
                const auto root = [&parents](u32 node) {
                    while (parents[node] != node) {
                        parents[node] = parents[parents[node]];
                        node = parents[node];
                    }
                    return node;
                };
                Array<Edge> skipped;
                PPR_DEFER {
                    if (known_scratch_peak != nullptr) {
                        *known_scratch_peak = std::max(
                            *known_scratch_peak, anchors.capacity() * sizeof(Anchor) +
                                                     (edges.capacity() + skipped.capacity()) * sizeof(Edge) +
                                                     parents.capacity() * sizeof(u32));
                    }
                };
                for (const auto &edge: edges) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, canceled(context));
                    const u32 a = root(edge.m_a), b = root(edge.m_b);
                    if (a == b) {
                        skipped.push_back(edge);
                        continue;
                    }
                    parents[b] = a;
                    PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, route(anchors[edge.m_a], anchors[edge.m_b]));
                }
                for (u32 i = 0u; i < std::min<std::size_t>(connection->m_extra_links, skipped.size()); ++i) {
                    const auto &edge = skipped[i];
                    PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, route(anchors[edge.m_a], anchors[edge.m_b]));
                }
                for (u32 i = 0u; i < connection->m_wells + connection->m_branches; ++i) {
                    bool admitted = false;
                    for (u32 attempt = 0u; attempt < 32u; ++attempt) {
                        const Anchor origin = anchors[static_cast<u32>(route_random.bounded(anchors.size()))];
                        const bool well = i < connection->m_wells;
                        const double angle = well ? -0.5 * std::numbers::pi
                                                  : route_random.unit() * 2.0 * std::numbers::pi;
                        const double length = well ? 35.0 + 55.0 * route_random.unit()
                                                   : 18.0 + 35.0 * route_random.unit();
                        const Anchor target{
                            origin.m_x + std::cos(angle) * length, origin.m_y + std::sin(angle) * length};
                        if (not clearLine(origin, target)) {
                            continue;
                        }
                        PPR_RETURN_ERROR_ON_FAIL(WorldGeometry, route(origin, target));
                        admitted = true;
                        break;
                    }
                    if (not admitted) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            WorldGeometry, make_error_code(EGenerationErrc::constraints_exhausted));
                    }
                }
            }
            return {};
        }
    }

    Expected<GenerationGeometry> compileGenerationGeometry(
        const GenerationPlan &plan, const BiomeLayout &layout, const SharedContext &context,
        u64 *const known_scratch_peak) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, canceled(context));
        GenerationGeometry geometry;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, features(plan, layout, geometry, context, known_scratch_peak));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, blobs(plan, layout, geometry, context, known_scratch_peak));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, worms(plan, layout, geometry, context));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, connections(plan, layout, geometry, context, known_scratch_peak));
        return geometry;
    }

    Expected<GenerationGeometry> compileBiomeSampleGeometry(
        const GenerationPlan &plan, const BiomeLayout &layout, const SharedContext &context,
        u64 *const known_scratch_peak) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, canceled(context));
        if (not layout.sampleBiome()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, std::errc::invalid_argument);
        }

        GenerationGeometry geometry;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, blobs(plan, layout, geometry, context, known_scratch_peak));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, worms(plan, layout, geometry, context));
        return geometry;
    }
}
