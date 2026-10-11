module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.matter;
import engine.math;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldMatter, info, none);
    namespace {
        struct Point {
            double m_x{}, m_y{};
        };
        struct Polygon {
            std::array<Point, 8u> m_points{};
            u32 m_size{};
        };
        [[nodiscard]] double coordinate(const Point point, const bool vertical) noexcept {
            return vertical ? point.m_y : point.m_x;
        }
        [[nodiscard]] Polygon clip(
            const Polygon &input, const bool vertical, const double boundary, const bool keep_greater) noexcept {
            Polygon output;
            if (input.m_size == 0u) {
                return output;
            }
            Point previous = input.m_points[input.m_size - 1u];
            const auto inside = [&](const Point point) {
                return keep_greater ? coordinate(point, vertical) >= boundary : coordinate(point, vertical) <= boundary;
            };
            bool previous_inside = inside(previous);
            for (u32 i = 0u; i < input.m_size; ++i) {
                const Point current = input.m_points[i];
                const bool current_inside = inside(current);
                if (previous_inside != current_inside) {
                    const double t = (boundary - coordinate(previous, vertical)) /
                                     (coordinate(current, vertical) - coordinate(previous, vertical));
                    PPR_ASSERT(output.m_size < output.m_points.size());
                    output.m_points[output.m_size++] = {
                        previous.m_x + t * (current.m_x - previous.m_x),
                        previous.m_y + t * (current.m_y - previous.m_y)};
                }
                if (current_inside) {
                    PPR_ASSERT(output.m_size < output.m_points.size());
                    output.m_points[output.m_size++] = current;
                }
                previous = current;
                previous_inside = current_inside;
            }
            return output;
        }
        [[nodiscard]] double area(const Polygon &polygon) noexcept {
            double twice = 0.0;
            for (u32 i = 0u; i < polygon.m_size; ++i) {
                const auto a = polygon.m_points[i], b = polygon.m_points[(i + 1u) % polygon.m_size];
                twice += a.m_x * b.m_y - a.m_y * b.m_x;
            }
            return std::abs(twice) * 0.5;
        }
        [[nodiscard]] bool solid(const ChunkGrid &grid, const i32 x, const i32 y) noexcept {
            if (x < 0 or y < 0 or x >= static_cast<i32>(kWorldEdge) or y >= static_cast<i32>(kWorldEdge)) {
                return true;
            }
            const auto ux = static_cast<u32>(x), uy = static_cast<u32>(y);
            const auto cells = grid.residentCells({ux / kChunkEdge, uy / kChunkEdge});
            return cells and (*cells)[(uy % kChunkEdge) * kChunkEdge + ux % kChunkEdge].m_element != 0u;
        }
        [[nodiscard]] double emptyArea(
            const ChunkGrid &grid, const GlobalCellPos cell, const double scale,
            const std::optional<double> fill_height) noexcept {
            double fine_height = 0.0;
            if (fill_height) {
                fine_height = *fill_height * scale;
                const double lattice = std::round(fine_height);
                if (*fill_height == lattice / scale) {
                    fine_height = lattice;
                }
            }
            const double top = fill_height ? std::clamp(fine_height - (cell.m_y + 0.5), -0.5, 0.5) : 0.5;
            if (top == -0.5) {
                return 0.0;
            }
            double occupied = 0.0;
            const auto points = math::contourPoints();
            const auto cases = math::contourCases();
            constexpr std::array<std::pair<i32, i32>, 4u> offsets{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
            for (i32 dy = -1; dy <= 0; ++dy) {
                for (i32 dx = -1; dx <= 0; ++dx) {
                    u32 mask = 0u;
                    for (u32 corner = 0u; corner < offsets.size(); ++corner) {
                        if (solid(
                                grid, static_cast<i32>(cell.m_x) + dx + offsets[corner].first,
                                static_cast<i32>(cell.m_y) + dy + offsets[corner].second)) {
                            mask |= 1u << corner;
                        }
                    }
                    const auto &shape = cases[mask];
                    for (u32 t = 0u; t < shape.m_triangle_count; ++t) {
                        const auto triangle = shape.m_triangles[t];
                        Polygon polygon;
                        for (const u32 index: {triangle.m_a, triangle.m_b, triangle.m_c}) {
                            polygon.m_points[polygon.m_size++] = {
                                dx + static_cast<double>(points[index].m_x),
                                dy + static_cast<double>(points[index].m_y)};
                        }
                        polygon = clip(polygon, false, -0.5, true);
                        polygon = clip(polygon, false, 0.5, false);
                        polygon = clip(polygon, true, -0.5, true);
                        polygon = clip(polygon, true, top, false);
                        occupied += area(polygon);
                    }
                }
            }
            return std::clamp(top + 0.5 - occupied, 0.0, top + 0.5) / scale / scale;
        }
        [[nodiscard]] std::error_code canceled(const IContext *const context) noexcept {
            return context != nullptr ? context->error() : std::error_code{};
        }
        [[nodiscard]] bool testBit(const Array<u64> &bits, const u32 index) noexcept {
            return (bits[index / 64u] & (1ull << (index % 64u))) != 0u;
        }
        void setBit(Array<u64> &bits, const u32 index) noexcept {
            bits[index / 64u] |= 1ull << (index % 64u);
        }
        [[nodiscard]] Expected<Array<u32>> component(
            const ChunkGrid &grid, const GenerationBounds bounds, const GlobalCellPos seed, InitialMatter &result,
            const IContext *const context) {
            const auto width = static_cast<u32>(bounds.m_max_x - bounds.m_min_x);
            const auto height = static_cast<u32>(bounds.m_max_y - bounds.m_min_y);
            Array<u64> visited((width * height + 63u) / 64u, 0ull);
            const auto local = [&](const i32 x, const i32 y) {
                return static_cast<u32>(y - bounds.m_min_y) * width + static_cast<u32>(x - bounds.m_min_x);
            };
            if (solid(grid, static_cast<i32>(seed.m_x), static_cast<i32>(seed.m_y))) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, make_error_code(EGenerationErrc::matter_anchor_solid));
            }
            Array<u32> queue;
            const auto append = [&](const i32 x, const i32 y) -> std::error_code {
                if (queue.size() >= kMaxMatterComponentCells) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldMatter, make_error_code(EGenerationErrc::matter_component_limit));
                }
                if (result.m_searched_cells >= kMaxMatterSearchCells) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldMatter, make_error_code(EGenerationErrc::matter_search_limit));
                }
                setBit(visited, local(x, y));
                queue.push_back(static_cast<u32>(y) * kWorldEdge + static_cast<u32>(x));
                ++result.m_searched_cells;
                return {};
            };
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, append(static_cast<i32>(seed.m_x), static_cast<i32>(seed.m_y)));
            for (std::size_t q = 0u; q < queue.size(); ++q) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, canceled(context));
                const auto x = static_cast<i32>(queue[q] % kWorldEdge), y = static_cast<i32>(queue[q] / kWorldEdge);
                // Shared midpoint contours join every adjacent empty sample.
                // In cases 5/10 the SOLIDS are disconnected, so the two empty
                // diagonal samples also share a positive-width opening. Other
                // diagonal empty pairs have an empty orthogonal intermediary.
                // Thus eight-neighbour sample connectivity matches the empty
                // complement of all sixteen contour cases. At the reservation
                // boundary, solid outside samples put the contour on or inside
                // its half-cell boundary; an adjacent outside empty sample leaks.
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if ((dx == 0 and dy == 0) or solid(grid, x + dx, y + dy)) {
                            continue;
                        }
                        const i32 nx = x + dx, ny = y + dy;
                        if (nx < bounds.m_min_x or
                            ny < bounds.m_min_y or
                            nx >= bounds.m_max_x or
                            ny >= bounds.m_max_y) {
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                WorldMatter, make_error_code(EGenerationErrc::matter_not_sealed));
                        }
                        if (not testBit(visited, local(nx, ny))) {
                            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, append(nx, ny));
                        }
                    }
                }
            }
            result.m_largest_component = std::max(result.m_largest_component, static_cast<u32>(queue.size()));
            std::ranges::sort(queue);
            return queue;
        }
        [[nodiscard]] std::error_code emit(
            const GenerationPlan &plan, const GenerationRule &rule, const GenerationFill &fill,
            const GenerationInstance &instance, const std::span<const u32> cells, const ChunkGrid &grid,
            Array<u64> &assigned, InitialMatter &result, const IContext *const context) {
            const double scale = 1.0 / plan.recipe().m_cell_size_m;
            InitialMatterRegion region{
                .m_identity = generationStreamSeed(plan.seed(), rule.m_key, instance.m_ordinal),
                .m_material = fill.m_material,
                .m_temperature_k = fill.m_temperature_k,
                .m_bulk_density_kg_m3 = fill.m_bulk_density_kg_m3};
            const std::optional<i64> boundary =
                fill.m_fill_height_m
                    ? std::optional<
                          i64>{static_cast<i64>(instance.m_origin_y) + static_cast<i64>(std::round(*fill.m_fill_height_m * scale))}
                    : std::nullopt;
            if (boundary) {
                region.m_fill_height_m = static_cast<double>(*boundary) / scale;
            }
            for (const u32 index: cells) {
                PPR_RETURN_ERROR_ON_FAIL(WorldMatter, canceled(context));
                if (testBit(assigned, index)) {
                    continue;
                }
                const GlobalCellPos cell{index % kWorldEdge, index / kWorldEdge};
                if (boundary and cell.m_y >= *boundary) {
                    continue;
                }
                // Integer boundaries select whole nominal cells; contour
                // intrusion still clips their eligible volume exactly.
                const double cell_area = emptyArea(grid, cell, scale, std::nullopt);
                if (cell_area == 0.0) {
                    continue;
                }
                const bool extend = not region.m_spans.empty() and
                                    region.m_spans.back().m_y == cell.m_y and
                                    region.m_spans.back().m_end_x == cell.m_x;
                if (extend) {
                    ++region.m_spans.back().m_end_x;
                } else {
                    if (result.m_span_count >= kMaxInitialMatterSpans) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldMatter, make_error_code(EGenerationErrc::matter_span_limit));
                    }
                    region.m_spans.push_back({cell.m_y, cell.m_x, cell.m_x + 1u});
                    ++result.m_span_count;
                }
                setBit(assigned, index);
                region.m_volume_m3 += cell_area * plan.recipe().m_reference_thickness_m;
            }
            if (region.m_spans.empty()) {
                return {};
            } // Expected absence after explicit fill precedence.
            if (result.m_regions.size() >= kMaxInitialMatterRegions) {
                PPR_RETURN_ERROR_ON_FAIL(WorldMatter, make_error_code(EGenerationErrc::matter_region_limit));
            }
            if (std::ranges::any_of(
                    result.m_regions, [&](const auto &prior) { return prior.m_identity == region.m_identity; })) {
                PPR_RETURN_ERROR_ON_FAIL(WorldMatter, std::errc::invalid_argument);
            }
            region.m_mass_kg = region.m_volume_m3 * region.m_bulk_density_kg_m3;
            if (not std::isfinite(region.m_mass_kg) or not std::isfinite(region.m_volume_m3)) {
                PPR_RETURN_ERROR_ON_FAIL(WorldMatter, std::errc::result_out_of_range);
            }
            result.m_regions.push_back(std::move(region));
            return {};
        }
    }
    Expected<double> generationEmptyArea(
        const ChunkGrid &grid, const GlobalCellPos cell, const double cell_size_m,
        const std::optional<double> fill_height_m) {
        if (cell.m_x >= kWorldEdge or
            cell.m_y >= kWorldEdge or
            not std::isfinite(cell_size_m) or
            cell_size_m <= 0.0 or
            not std::isfinite(cell_size_m * cell_size_m) or
            cell_size_m * cell_size_m == 0.0 or
            not std::isfinite(1.0 / cell_size_m) or
            (fill_height_m and (not std::isfinite(*fill_height_m) or
                                   *fill_height_m < 0.0 or
                                   *fill_height_m > kWorldEdge * cell_size_m))) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, std::errc::invalid_argument);
        }
        const double result = emptyArea(grid, cell, 1.0 / cell_size_m, fill_height_m);
        if (not std::isfinite(result)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, std::errc::result_out_of_range);
        }
        return result;
    }
    Expected<InitialMatter> resolveInitialMatter(
        const GenerationPlan &plan, const GenerationGeometry &geometry, const ChunkGrid &grid,
        const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, canceled(context));
        InitialMatter result;
        const auto &recipe = plan.recipe();
        Array<u64> assigned(kWorldEdge * kWorldEdge / 64u, 0ull);
        // Non-gas rules resolve their authored precedence first. Gas is the
        // remainder of those assignments, retaining authored order among gases.
        for (const bool gas: {false, true}) {
            for (const auto &rule: recipe.m_rules) {
                const auto *const fill = std::get_if<GenerationFill>(&rule.m_parameters);
                if (fill == nullptr or fill->m_gas != gas) {
                    continue;
                }
                for (const auto &instance: geometry.m_instances) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, canceled(context));
                    if (instance.m_rule != fill->m_placement_rule) {
                        continue;
                    }
                    if (instance.m_feature >= recipe.m_features.size()) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, std::errc::invalid_argument);
                    }
                    const auto &feature = recipe.m_features[instance.m_feature];
                    if (fill->m_marker >= feature.m_markers.size()) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, std::errc::invalid_argument);
                    }
                    const auto &local = feature.m_bounds;
                    const i64 left = static_cast<i64>(instance.m_origin_x) + local.m_min_x;
                    const i64 bottom = static_cast<i64>(instance.m_origin_y) + local.m_min_y;
                    const i64 right = static_cast<i64>(instance.m_origin_x) + local.m_max_x;
                    const i64 top = static_cast<i64>(instance.m_origin_y) + local.m_max_y;
                    if (left < 0 or bottom < 0 or right > kWorldEdge or top > kWorldEdge) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, std::errc::invalid_argument);
                    }
                    const auto &marker = feature.m_markers[fill->m_marker];
                    const i32 marker_x = instance.m_reflected ? local.m_min_x + local.m_max_x - marker.m_x - 1
                                                              : marker.m_x;
                    const GlobalCellPos seed{
                        static_cast<u32>(instance.m_origin_x + marker_x),
                        static_cast<u32>(instance.m_origin_y + marker.m_y)};
                    auto cells = component(
                        grid,
                        {static_cast<i32>(left), static_cast<i32>(bottom), static_cast<i32>(right),
                            static_cast<i32>(top)},
                        seed, result, context);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldMatter, cells);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldMatter, emit(plan, rule, *fill, instance, *cells, grid, assigned, result, context));
                }
            }
        }
        return result;
    }
}
