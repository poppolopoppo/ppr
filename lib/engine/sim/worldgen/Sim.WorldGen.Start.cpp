module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.start;
import :worldgen.layout;
import engine.math;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldStart, info, none);
    namespace {
        struct Point {
            double m_x{}, m_y{};
        };
        struct Triangle {
            std::array<Point, 3u> m_points;
        };
        struct Support {
            double m_y{}, m_left{}, m_right{};
        };
        [[nodiscard]] std::error_code canceled(const IContext *const context) noexcept {
            return context != nullptr ? context->error() : std::error_code{};
        }
        [[nodiscard]] u16 material(const ChunkGrid &grid, const i32 x, const i32 y) noexcept {
            if (x < 0 or y < 0 or x >= static_cast<i32>(kWorldEdge) or y >= static_cast<i32>(kWorldEdge)) {
                return 65535u;
            }
            const auto ux = static_cast<u32>(x), uy = static_cast<u32>(y);
            const auto cells = grid.residentCells({ux / kChunkEdge, uy / kChunkEdge});
            return cells ? (*cells)[(uy % kChunkEdge) * kChunkEdge + ux % kChunkEdge].m_element : u16{0u};
        }
        [[nodiscard]] bool overlaps(
            const Triangle &triangle, const double left, const double right, const double bottom,
            const double top) noexcept {
            const auto separated = [&](const double nx, const double ny) {
                double low = (std::numeric_limits<double>::max)(), high = -low;
                for (const auto point: triangle.m_points) {
                    const double projection = point.m_x * nx + point.m_y * ny;
                    low = std::min(low, projection);
                    high = std::max(high, projection);
                }
                const double box_low = (nx >= 0.0 ? left : right) * nx + (ny >= 0.0 ? bottom : top) * ny;
                const double box_high = (nx >= 0.0 ? right : left) * nx + (ny >= 0.0 ? top : bottom) * ny;
                return high <= box_low or box_high <= low;
            };
            if (separated(1.0, 0.0) or separated(0.0, 1.0)) {
                return false;
            }
            for (u32 i = 0u; i < 3u; ++i) {
                const auto a = triangle.m_points[i], b = triangle.m_points[(i + 1u) % 3u];
                if (separated(a.m_y - b.m_y, b.m_x - a.m_x)) {
                    return false;
                }
            }
            return true;
        }
        [[nodiscard]] Expected<u32> resourceCells(
            const GenerationRecipe &recipe, const GenerationResource &resource, const ChunkGrid &grid,
            const GenerationBounds bounds, const double spawn_x, const double spawn_y, const double floor_y,
            const double route_left, const double route_right, const IContext *const context, const u64 contour_bytes,
            u64 *const known_scratch_peak) {
            const auto &marker = recipe.m_features[recipe.m_start_feature].m_markers[resource.m_marker];
            const i32 seed_x = static_cast<i32>(recipe.m_start_x_fine) + marker.m_x;
            const i32 seed_y = static_cast<i32>(recipe.m_start_y_fine) + marker.m_y;
            if (material(grid, seed_x, seed_y) != resource.m_material) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, make_error_code(EGenerationErrc::start_invalid));
            }
            const double scale = 1.0 / recipe.m_cell_size_m;
            const double limit = resource.m_max_distance_m * scale;
            const double anchor_dx = seed_x + 0.5 - spawn_x, anchor_dy = seed_y + 0.5 - spawn_y;
            if (std::hypot(anchor_dx, anchor_dy) > limit) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, make_error_code(EGenerationErrc::start_invalid));
            }
            const auto width = static_cast<u32>(bounds.m_max_x - bounds.m_min_x);
            const auto height = static_cast<u32>(bounds.m_max_y - bounds.m_min_y);
            Array<u64> visited((width * height + 63u) / 64u, 0ull);
            Array<GlobalCellPos> queue;
            PPR_DEFER {
                if (known_scratch_peak != nullptr) {
                    *known_scratch_peak = std::max(
                        *known_scratch_peak,
                        contour_bytes + visited.capacity() * sizeof(u64) + queue.capacity() * sizeof(GlobalCellPos));
                }
            };
            const auto append = [&](const i32 x, const i32 y) {
                const auto index = static_cast<u32>(y - bounds.m_min_y) * width + static_cast<u32>(x - bounds.m_min_x);
                const u64 bit = 1ull << (index % 64u);
                if ((visited[index / 64u] & bit) == 0u) {
                    visited[index / 64u] |= bit;
                    queue.push_back({static_cast<u32>(x), static_cast<u32>(y)});
                }
            };
            append(seed_x, seed_y);
            u32 count = 0u;
            bool exposed = false;
            constexpr std::array<std::pair<i32, i32>, 4u> neighbours{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
            for (std::size_t i = 0u; i < queue.size(); ++i) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, canceled(context));
                const auto cell = queue[i];
                const double dx = cell.m_x + 0.5 - spawn_x, dy = cell.m_y + 0.5 - spawn_y;
                if (std::hypot(dx, dy) <= limit) {
                    ++count;
                }
                exposed = exposed or (std::hypot(dx, dy) <= limit and
                                         cell.m_y + 1.0 == floor_y and
                                         cell.m_x + 1.0 > route_left and
                                         cell.m_x < route_right);
                for (const auto [ox, oy]: neighbours) {
                    const i32 x = static_cast<i32>(cell.m_x) + ox, y = static_cast<i32>(cell.m_y) + oy;
                    if (x >= bounds.m_min_x and
                        y >= bounds.m_min_y and
                        x < bounds.m_max_x and
                        y < bounds.m_max_y and
                        material(grid, x, y) == resource.m_material) {
                        append(x, y);
                    }
                }
            }
            if (not exposed or count < resource.m_minimum_fine_cells) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, make_error_code(EGenerationErrc::start_invalid));
            }
            return count;
        }
    }
    Expected<GenerationStart> validateGenerationStart(
        const GenerationPlan &plan, const ChunkGrid &grid, const IContext *const context,
        u64 *const known_scratch_peak) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, canceled(context));
        const auto &recipe = plan.recipe();
        const auto &feature = recipe.m_features[recipe.m_start_feature];
        const GenerationBounds bounds{
            static_cast<i32>(recipe.m_start_x_fine) + feature.m_bounds.m_min_x,
            static_cast<i32>(recipe.m_start_y_fine) + feature.m_bounds.m_min_y,
            static_cast<i32>(recipe.m_start_x_fine) + feature.m_bounds.m_max_x,
            static_cast<i32>(recipe.m_start_y_fine) + feature.m_bounds.m_max_y};
        const auto width = static_cast<u32>(bounds.m_max_x - bounds.m_min_x + 1);
        const auto height = static_cast<u32>(bounds.m_max_y - bounds.m_min_y + 1);
        if (static_cast<u64>(width) * height > kMaxStartContourCells) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, make_error_code(EGenerationErrc::start_contour_limit));
        }
        const double scale = 1.0 / recipe.m_cell_size_m;
        const auto &marker = feature.m_markers[recipe.m_start_marker];
        const double spawn_x = recipe.m_start_x_fine + marker.m_x + 0.5;
        const double marker_y = recipe.m_start_y_fine + marker.m_y + 0.5;
        double route_left = spawn_x, route_right = spawn_x;
        for (const auto &resource: recipe.m_resources) {
            const double x = recipe.m_start_x_fine + feature.m_markers[resource.m_marker].m_x + 0.5;
            route_left = std::min(route_left, x);
            route_right = std::max(route_right, x);
        }
        const double half_width = recipe.m_start_route_width_m * scale * 0.5;
        route_left -= half_width;
        route_right += half_width;
        Array<Support> supports;
        Array<Triangle> triangles;
        PPR_DEFER {
            if (known_scratch_peak != nullptr) {
                *known_scratch_peak = std::max(
                    *known_scratch_peak,
                    supports.capacity() * sizeof(Support) + triangles.capacity() * sizeof(Triangle));
            }
        };
        const auto points = math::contourPoints();
        const auto cases = math::contourCases();
        constexpr std::array<std::pair<i32, i32>, 4u> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (i32 y = bounds.m_min_y - 1; y < bounds.m_max_y; ++y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, canceled(context));
            for (i32 x = bounds.m_min_x - 1; x < bounds.m_max_x; ++x) {
                u32 mask = 0u;
                for (u32 c = 0u; c < corners.size(); ++c) {
                    if (material(grid, x + corners[c].first, y + corners[c].second) != 0u) {
                        mask |= 1u << c;
                    }
                }
                const auto &shape = cases[mask];
                const auto point = [&](const u32 index) -> Point {
                    return {x + 0.5 + points[index].m_x, y + 0.5 + points[index].m_y};
                };
                for (u32 s = 0u; s < shape.m_segment_count; ++s) {
                    const auto a = point(shape.m_segments[s].m_begin), b = point(shape.m_segments[s].m_end);
                    if (a.m_y == b.m_y and a.m_x > b.m_x and a.m_y <= marker_y) {
                        supports.push_back({a.m_y, b.m_x, a.m_x});
                    }
                }
                for (u32 t = 0u; t < shape.m_triangle_count; ++t) {
                    const auto triangle = shape.m_triangles[t];
                    triangles.push_back({{point(triangle.m_a), point(triangle.m_b), point(triangle.m_c)}});
                }
            }
        }
        std::ranges::sort(supports, [](const auto &a, const auto &b) {
            return a.m_y != b.m_y ? a.m_y > b.m_y : a.m_left < b.m_left;
        });
        std::optional<double> floor;
        const double body_half_width = recipe.m_actor_half_width_m * scale;
        for (std::size_t i = 0u; i < supports.size();) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, canceled(context));
            const double level = supports[i].m_y;
            double left = supports[i].m_left, right = supports[i].m_right;
            const auto accepts = [&] {
                return left <= spawn_x - body_half_width and right >= spawn_x + body_half_width;
            };
            for (++i; i < supports.size() and supports[i].m_y == level; ++i) {
                if (supports[i].m_left > right) {
                    if (accepts()) {
                        floor = level;
                        break;
                    }
                    left = supports[i].m_left;
                }
                right = std::max(right, supports[i].m_right);
            }
            if (floor or accepts()) {
                floor = level;
                if (left > route_left or right < route_right) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, make_error_code(EGenerationErrc::start_invalid));
                }
                break;
            }
        }
        if (not floor or
            route_left < bounds.m_min_x or
            route_right > bounds.m_max_x or
            *floor + recipe.m_start_clearance_m * scale > bounds.m_max_y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, make_error_code(EGenerationErrc::start_invalid));
        }
        for (const auto &triangle: triangles) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, canceled(context));
            if (overlaps(triangle, route_left, route_right, *floor, *floor + recipe.m_start_clearance_m * scale)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, make_error_code(EGenerationErrc::start_invalid));
            }
        }
        const double spawn_y = *floor + recipe.m_actor_half_height_m * scale;
        GenerationStart result{spawn_x / scale, spawn_y / scale, *floor / scale};
        for (const auto &resource: recipe.m_resources) {
            auto count = resourceCells(
                recipe, resource, grid, bounds, spawn_x, spawn_y, *floor, route_left, route_right, context,
                supports.capacity() * sizeof(Support) + triangles.capacity() * sizeof(Triangle), known_scratch_peak);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldStart, count);
            result.m_resource_fine_cells.push_back(*count);
        }
        return result;
    }
}
