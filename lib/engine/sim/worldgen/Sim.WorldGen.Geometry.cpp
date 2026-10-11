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
    namespace {
        [[nodiscard]] std::error_code canceled(const IContext *const context) noexcept {
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
            const GenerationRecipe &recipe, const GenerationGeometry &geometry,
            const GenerationInstance &candidate) noexcept {
            const Rect bounds = reservation(recipe, candidate);
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
            const u32 ordinal, const IContext *const context) {
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
                const double depth = 1.0 - 0.5 * static_cast<double>(rect.m_bottom + rect.m_top) / kWorldEdge;
                if (depth < placement.m_depth_min or
                    depth > placement.m_depth_max or
                    generationBiomeAt(plan, layout, x, y) != placement.m_biome or
                    not fits(recipe, geometry, candidate)) {
                    continue;
                }
                geometry.m_instances.push_back(candidate);
                return true;
            }
            return false;
        }
        [[nodiscard]] std::error_code features(
            const GenerationPlan &plan, const BiomeLayout &layout, GenerationGeometry &geometry,
            const IContext *const context, u64 *const known_scratch_peak) {
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
        [[nodiscard]] std::error_code worms(
            const GenerationPlan &plan, const BiomeLayout &layout, GenerationGeometry &geometry,
            const IContext *const context) {
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
                    for (u32 attempt = 0u; attempt < 64u; ++attempt) {
                        x = boundary + (extent - 2.0 * boundary) * random.unit();
                        y = boundary + (extent - 2.0 * boundary) * random.unit();
                        const double depth = 1.0 - y / extent;
                        if (depth >= worm->m_depth_min and
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
            const GenerationPlan &plan, GenerationGeometry &geometry, const IContext *const context,
            u64 *const known_scratch_peak) {
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
                if (connection == nullptr or anchors.size() < 2u) {
                    continue;
                }
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
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldGeometry,
                        addSegment(
                            geometry, {r, anchors[edge.m_a].m_x, anchors[edge.m_a].m_y, anchors[edge.m_b].m_x,
                                          anchors[edge.m_b].m_y, connection->m_radius_m}));
                }
                for (u32 i = 0u; i < std::min<std::size_t>(connection->m_extra_links, skipped.size()); ++i) {
                    const auto &edge = skipped[i];
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldGeometry,
                        addSegment(
                            geometry, {r, anchors[edge.m_a].m_x, anchors[edge.m_a].m_y, anchors[edge.m_b].m_x,
                                          anchors[edge.m_b].m_y, connection->m_radius_m}));
                }
            }
            return {};
        }
    }
    Expected<GenerationGeometry> compileGenerationGeometry(
        const GenerationPlan &plan, const BiomeLayout &layout, const IContext *const context,
        u64 *const known_scratch_peak) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, canceled(context));
        GenerationGeometry geometry;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, features(plan, layout, geometry, context, known_scratch_peak));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, worms(plan, layout, geometry, context));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldGeometry, connections(plan, geometry, context, known_scratch_peak));
        return geometry;
    }
}
