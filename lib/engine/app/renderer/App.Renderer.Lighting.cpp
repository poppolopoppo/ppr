module;
#include "pP/Macros.h"

module engine.app;

import :renderer.lighting;

import engine.core;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Lighting, debug, none)
}

namespace pP::lighting {
    namespace {
        // ------------------------------------------------------------------
        // reference validation and numerical operations
        // ------------------------------------------------------------------

        std::error_code require_(const bool valid, const std::errc error = std::errc::invalid_argument) {
            return valid ? std::error_code{} : std::make_error_code(error);
        }

        bool validRgb_(const OpticalRgb &rgb) noexcept {
            return std::ranges::all_of(rgb, [](const double v) { return std::isfinite(v) and v >= 0.0; });
        }

        bool zeroRgb_(const OpticalRgb &rgb) noexcept {
            return std::ranges::all_of(rgb, [](const double v) { return v == 0.0; });
        }

        bool validInterval_(const RadianceInterval &interval) noexcept {
            return validRgb_(interval.m_radiance) and
                   validRgb_(interval.m_transmittance) and
                   std::ranges::all_of(interval.m_transmittance, [](const double v) { return v <= 1.0; });
        }

        std::error_code multiply_(const u64 a, const u64 b, u64 &result) {
            PPR_RETURN_ERROR_ON_FAIL(
                Lighting, require_(b == 0u or a <= std::numeric_limits<u64>::max() / b, std::errc::value_too_large));
            result = a * b;
            return {};
        }

        std::error_code add_(const u64 a, const u64 b, u64 &result) {
            PPR_RETURN_ERROR_ON_FAIL(
                Lighting, require_(a <= std::numeric_limits<u64>::max() - b, std::errc::value_too_large));
            result = a + b;
            return {};
        }

        std::error_code validateGrid_(const OpticalGridView &grid, std::array<double, 2> &maximum) {
            PPR_RETURN_ERROR_ON_FAIL(
                Lighting,
                require_(
                    grid.m_width > 0u and
                    grid.m_height > 0u and
                    std::isfinite(grid.m_cell_size_m) and
                    grid.m_cell_size_m > 0.0 and
                    (grid.m_boundary == OpticalBoundary::vacuum or grid.m_boundary == OpticalBoundary::opaque)));

            u64 count{};
            PPR_RETURN_ERROR_ON_FAIL(Lighting, multiply_(grid.m_width, grid.m_height, count));
            PPR_RETURN_ERROR_ON_FAIL(Lighting, require_(count == grid.m_cells.size()));

            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                const u32 extent = axis == 0u ? grid.m_width : grid.m_height;
                maximum[axis] = grid.m_min_m[axis] + double(extent) * grid.m_cell_size_m;
                PPR_RETURN_ERROR_ON_FAIL(
                    Lighting, require_(
                                  std::isfinite(grid.m_min_m[axis]) and
                                  std::isfinite(maximum[axis]) and
                                  maximum[axis] > grid.m_min_m[axis] and
                                  grid.m_min_m[axis] + grid.m_cell_size_m > grid.m_min_m[axis] and
                                  maximum[axis] - grid.m_cell_size_m < maximum[axis]));

                // Endpoint spacing is insufficient at large translations: an
                // interior adjacent pair may still collapse after rounding.
                double previous = grid.m_min_m[axis];
                for (u64 index = 1u; index <= extent; ++index) {
                    const double plane = grid.m_min_m[axis] + double(index) * grid.m_cell_size_m;
                    PPR_RETURN_ERROR_ON_FAIL(Lighting, require_(plane > previous));
                    previous = plane;
                }
            }

            // Validate the entire supplied optical field; unavailable is never vacuum.
            for (const OpticalCell &cell: grid.m_cells) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Lighting, require_(cell.m_kind != OpticalKind::unavailable, std::errc::no_message));
                const bool vacuum = cell.m_kind == OpticalKind::vacuum and
                                    zeroRgb_(cell.m_extinction) and
                                    zeroRgb_(cell.m_volume_emission) and
                                    zeroRgb_(cell.m_surface_emission);
                const bool opaque = cell.m_kind == OpticalKind::opaque and
                                    zeroRgb_(cell.m_extinction) and
                                    zeroRgb_(cell.m_volume_emission) and
                                    validRgb_(cell.m_surface_emission);
                const bool absorbing = cell.m_kind == OpticalKind::absorbing and
                                       validRgb_(cell.m_extinction) and
                                       validRgb_(cell.m_volume_emission) and
                                       zeroRgb_(cell.m_surface_emission);
                PPR_RETURN_ERROR_ON_FAIL(Lighting, require_(vacuum or opaque or absorbing));
            }

            return {};
        }

        std::error_code validateRay_(const OpticalRay &ray, std::array<double, 2> &direction) {
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Lighting, require_(std::isfinite(ray.m_origin_m[axis]) and std::isfinite(ray.m_direction[axis])));
            }

            const double length = std::hypot(ray.m_direction[0], ray.m_direction[1]);
            PPR_RETURN_ERROR_ON_FAIL(
                Lighting, require_(
                              std::isfinite(length) and
                              length > 0.0 and
                              std::isfinite(ray.m_near_m) and
                              std::isfinite(ray.m_far_m) and
                              ray.m_near_m >= 0.0 and
                              ray.m_far_m >= ray.m_near_m));

            direction = {ray.m_direction[0] / length, ray.m_direction[1] / length};
            return {};
        }

        std::expected<RadianceInterval, std::error_code> integrateCell_(
            const OpticalCell &cell, const double distance_m) {
            RadianceInterval local{};
            if (cell.m_kind == OpticalKind::opaque) {
                local.m_radiance = cell.m_surface_emission;
                local.m_transmittance = {};
            } else if (cell.m_kind == OpticalKind::absorbing) {
                for (std::size_t channel = 0u; channel < 3u; ++channel) {
                    const double extinction = cell.m_extinction[channel];
                    const double depth = extinction * distance_m;
                    local.m_transmittance[channel] = std::exp(-depth);
                    const double integral = extinction == 0.0 ? distance_m : -std::expm1(-depth) / extinction;
                    local.m_radiance[channel] = cell.m_volume_emission[channel] * integral;
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, require_(validInterval_(local), std::errc::result_out_of_range));
            return local;
        }

        // Each axis search assigns the cell occupied immediately AFTER t.
        // Searching plane events, rather than rounding origin+t*direction, gives
        // negative-boundary/corner ties the same ownership as the slab oracle.
        std::expected<u32, std::error_code> cellAt_(
            const double t, const double origin, const double direction, const double minimum, const double cell_size,
            const u32 extent) {
            u64 low{};
            u64 high = u64{extent} + 1u;
            while (low < high) {
                const u64 middle = low + (high - low) / 2u;
                const double plane = minimum + double(middle) * cell_size;
                const double event = direction == 0.0 ? plane : (plane - origin) / direction;
                const bool passed = direction == 0.0 ? event <= origin : event <= t;
                if (direction < 0.0 ? not passed : passed) {
                    low = middle + 1u;
                } else {
                    high = middle;
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Lighting, require_(low > 0u and low <= extent, std::errc::result_out_of_range));
            return static_cast<u32>(low - 1u);
        }

        bool intersect_(
            const OpticalRay &ray, const std::array<double, 2> &direction, const std::array<double, 2> &minimum,
            const std::array<double, 2> &maximum, double &near_m, double &far_m) noexcept {
            near_m = ray.m_near_m;
            far_m = ray.m_far_m;
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                if (direction[axis] == 0.0) {
                    if (ray.m_origin_m[axis] < minimum[axis] or ray.m_origin_m[axis] >= maximum[axis]) {
                        return false;
                    }
                    continue;
                }

                const double a = (minimum[axis] - ray.m_origin_m[axis]) / direction[axis];
                const double b = (maximum[axis] - ray.m_origin_m[axis]) / direction[axis];
                near_m = std::max(near_m, std::min(a, b));
                far_m = std::min(far_m, std::max(a, b));
            }

            return near_m < far_m;
        }
    }

    // ------------------------------------------------------------------
    // ordered radiance transport
    // ------------------------------------------------------------------

    std::error_code validateOpticalGrid(const OpticalGridView &grid) {
        std::array<double, 2> maximum{};
        PPR_RETURN_ERROR_ON_FAIL(Lighting, validateGrid_(grid, maximum));
        return {};
    }

    std::expected<OpticalRay, std::error_code> normalizeOpticalRay(const OpticalRay &ray) {
        OpticalRay normalized = ray;
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, validateRay_(ray, normalized.m_direction));
        return normalized;
    }

    std::expected<RadianceInterval, std::error_code> composeIntervals(
        const RadianceInterval &near_interval, const RadianceInterval &far_interval) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, require_(validInterval_(near_interval) and validInterval_(far_interval)));

        RadianceInterval result{};
        for (std::size_t channel = 0u; channel < 3u; ++channel) {
            result.m_radiance[channel] = near_interval.m_radiance[channel] +
                                         near_interval.m_transmittance[channel] * far_interval.m_radiance[channel];
            result.m_transmittance[channel] = near_interval.m_transmittance[channel] *
                                              far_interval.m_transmittance[channel];
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, require_(validInterval_(result), std::errc::result_out_of_range));
        return result;
    }

    std::expected<RadianceInterval, std::error_code> traceOpticalReference(
        const OpticalGridView &grid, const OpticalRay &ray) {
        const hal::ProfileScope scope{"App.Lighting.Reference"};
        std::array<double, 2> maximum{};
        std::array<double, 2> direction{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, validateGrid_(grid, maximum));
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, validateRay_(ray, direction));

        RadianceInterval result{};
        if (ray.m_near_m == ray.m_far_m) {
            return result;
        }

        double world_near{};
        double world_far{};
        const bool hits_world = intersect_(ray, direction, grid.m_min_m, maximum, world_near, world_far);
        if (grid.m_boundary == OpticalBoundary::opaque and (not hits_world or world_near > ray.m_near_m)) {
            result.m_transmittance = {};
            return result;
        }

        struct Segment {
            double m_near{};
            double m_far{};
            std::size_t m_cell{};
        };
        std::vector<Segment> segments{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, require_(grid.m_cells.size() <= segments.max_size(), std::errc::value_too_large));
        for (u32 y = 0u; y < grid.m_height; ++y) {
            for (u32 x = 0u; x < grid.m_width; ++x) {
                const std::array<double, 2> minimum{
                    grid.m_min_m[0] + double(x) * grid.m_cell_size_m, grid.m_min_m[1] + double(y) * grid.m_cell_size_m};
                const std::array<double, 2> cell_maximum{
                    grid.m_min_m[0] + (double(x) + 1.0) * grid.m_cell_size_m,
                    grid.m_min_m[1] + (double(y) + 1.0) * grid.m_cell_size_m};
                double near_m{};
                double far_m{};
                if (not intersect_(ray, direction, minimum, cell_maximum, near_m, far_m)) {
                    continue;
                }

                segments.push_back({near_m, far_m, std::size_t(y) * grid.m_width + x});
            }
        }

        std::ranges::sort(segments, {}, &Segment::m_near);

        for (const Segment &segment: segments) {
            const OpticalCell &cell = grid.m_cells[segment.m_cell];
            const auto local = integrateCell_(cell, segment.m_far - segment.m_near);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, local ? std::error_code{} : local.error());

            auto composed = composeIntervals(result, *local);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, composed ? std::error_code{} : composed.error());
            result = *composed;
            if (cell.m_kind == OpticalKind::opaque) {
                return result;
            }
        }

        if (grid.m_boundary == OpticalBoundary::opaque and hits_world and world_far < ray.m_far_m) {
            result.m_transmittance = {};
        }
        return result;
    }

    // ------------------------------------------------------------------
    // immutable hierarchy capture
    // ------------------------------------------------------------------

    std::expected<OpticalHierarchy, std::error_code> OpticalHierarchy::capture(const OpticalGridView &grid) {
        const hal::ProfileScope scope{"App.Lighting.HierarchyCapture"};
        std::array<double, 2> maximum{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, validateGrid_(grid, maximum));

        OpticalHierarchy hierarchy{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, require_(grid.m_cells.size() <= hierarchy.m_cells.max_size(), std::errc::value_too_large));
        hierarchy.m_cells.assign(grid.m_cells.begin(), grid.m_cells.end());
        hierarchy.m_min_m = grid.m_min_m;
        hierarchy.m_cell_size_m = grid.m_cell_size_m;
        hierarchy.m_width = grid.m_width;
        hierarchy.m_height = grid.m_height;
        hierarchy.m_boundary = grid.m_boundary;

        std::vector<u8> base{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, require_(grid.m_cells.size() <= base.max_size(), std::errc::value_too_large));
        base.reserve(grid.m_cells.size());
        for (const OpticalCell &cell: grid.m_cells) {
            base.push_back(cell.m_kind == OpticalKind::vacuum ? 1u : 0u);
        }

        hierarchy.m_vacuum_levels.push_back(std::move(base));
        u64 width = grid.m_width;
        u64 height = grid.m_height;
        while (width > 1u or height > 1u) {
            const u64 parent_width = (width + 1u) / 2u;
            const u64 parent_height = (height + 1u) / 2u;
            u64 count{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, multiply_(parent_width, parent_height, count));

            std::vector<u8> parent{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, require_(count <= parent.max_size(), std::errc::value_too_large));
            parent.resize(static_cast<std::size_t>(count));
            const auto &children = hierarchy.m_vacuum_levels.back();
            for (u64 y = 0u; y < parent_height; ++y) {
                for (u64 x = 0u; x < parent_width; ++x) {
                    // Missing children are outside, never silently vacuum.
                    if (2u * x + 1u >= width or 2u * y + 1u >= height) {
                        continue;
                    }

                    const u64 first = 2u * y * width + 2u * x;
                    const bool empty = children[static_cast<std::size_t>(first)] != 0u and
                                       children[static_cast<std::size_t>(first + 1u)] != 0u and
                                       children[static_cast<std::size_t>(first + width)] != 0u and
                                       children[static_cast<std::size_t>(first + width + 1u)] != 0u;
                    parent[static_cast<std::size_t>(y * parent_width + x)] = empty ? 1u : 0u;
                }
            }

            hierarchy.m_vacuum_levels.push_back(std::move(parent));
            width = parent_width;
            height = parent_height;
        }

        return hierarchy;
    }

    // ------------------------------------------------------------------
    // scalar hierarchical DDA
    // ------------------------------------------------------------------

    std::expected<OpticalTraceResult, std::error_code> traceOpticalAccelerated(
        const OpticalHierarchy &hierarchy, const OpticalRay &ray) {
        const hal::ProfileScope scope{"App.Lighting.HierarchicalDda"};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, require_(
                          hierarchy.m_width > 0u and
                          hierarchy.m_height > 0u and
                          not hierarchy.m_vacuum_levels.empty() and
                          hierarchy.m_cells.size() == u64{hierarchy.m_width} * hierarchy.m_height));
        std::array<double, 2> direction{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, validateRay_(ray, direction));

        OpticalTraceResult result{};
        if (ray.m_near_m == ray.m_far_m) {
            return result;
        }

        const std::array<double, 2> maximum{
            hierarchy.m_min_m[0] + double(hierarchy.m_width) * hierarchy.m_cell_size_m,
            hierarchy.m_min_m[1] + double(hierarchy.m_height) * hierarchy.m_cell_size_m};
        double world_near{};
        double world_far{};
        const bool hits_world = intersect_(ray, direction, hierarchy.m_min_m, maximum, world_near, world_far);
        if (hierarchy.m_boundary == OpticalBoundary::opaque and (not hits_world or world_near > ray.m_near_m)) {
            result.m_interval.m_transmittance = {};
            return result;
        }

        if (not hits_world) {
            return result;
        }

        double cursor = world_near;
        while (cursor < world_far) {
            std::array<u32, 2> cell{};
            const std::array<u32, 2> extent{hierarchy.m_width, hierarchy.m_height};
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                const auto located = cellAt_(
                    cursor, ray.m_origin_m[axis], direction[axis], hierarchy.m_min_m[axis], hierarchy.m_cell_size_m,
                    extent[axis]);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, located ? std::error_code{} : located.error());
                cell[axis] = *located;
            }

            // All ancestors are aligned. A nonempty leaf may still have vacuum
            // neighbors, but no ancestor containing it can safely be skipped.
            u64 block_size = 1u;
            bool vacuum = false;
            for (std::size_t level = hierarchy.m_vacuum_levels.size(); level-- > 0u;) {
                const u64 size = u64{1} << level;
                const u64 level_width = (u64{hierarchy.m_width} + size - 1u) / size;
                const u64 index = (u64{cell[1]} / size) * level_width + u64{cell[0]} / size;
                if (hierarchy.m_vacuum_levels[level][static_cast<std::size_t>(index)] == 0u) {
                    continue;
                }

                block_size = size;
                vacuum = true;
                break;
            }

            double next = world_far;
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                if (direction[axis] == 0.0) {
                    continue;
                }

                const u64 block_minimum = (u64{cell[axis]} / block_size) * block_size;
                const u64 exit_index = direction[axis] > 0.0 ? block_minimum + block_size : block_minimum;
                const double plane = hierarchy.m_min_m[axis] + double(exit_index) * hierarchy.m_cell_size_m;
                const double exit = (plane - ray.m_origin_m[axis]) / direction[axis];
                next = std::min(next, exit);
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, require_(next > cursor, std::errc::result_out_of_range));
            ++result.m_segments;
            // Every positive segment ends at a previously unpassed X or Y plane
            // (or the interval end). No ray can consume more than width+height.
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Lighting, require_(
                              result.m_segments <= u64{hierarchy.m_width} + hierarchy.m_height,
                              std::errc::state_not_recoverable));
            if (vacuum) {
                ++result.m_vacuum_blocks;
            } else {
                const OpticalCell &optical = hierarchy.m_cells[std::size_t(cell[1]) * hierarchy.m_width + cell[0]];
                const auto local = integrateCell_(optical, next - cursor);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, local ? std::error_code{} : local.error());

                auto composed = composeIntervals(result.m_interval, *local);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, composed ? std::error_code{} : composed.error());
                result.m_interval = *composed;
                if (optical.m_kind == OpticalKind::opaque) {
                    return result;
                }
            }

            cursor = next;
        }

        if (hierarchy.m_boundary == OpticalBoundary::opaque and world_far < ray.m_far_m) {
            result.m_interval.m_transmittance = {};
        }
        return result;
    }

    // ------------------------------------------------------------------
    // checked RC layout and live-storage accounting
    // ------------------------------------------------------------------

    std::expected<RcResourceEstimate, std::error_code> estimateRcResources(const RcEstimateRequest &request) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, require_(
                          request.m_width > 0u and
                          request.m_height > 0u and
                          request.m_cascade_count > 0u and
                          request.m_cascade_count <= 61u and
                          request.m_live_generations > 0u and
                          request.m_c0_tile_width > 0u and
                          request.m_c0_tile_height > 0u and
                          request.m_interval_bytes > 0u and
                          request.m_additional_largest_resource_bytes <= request.m_additional_peak_bytes));
        const u64 outer = u64{1} << (request.m_cascade_count - 1u);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, require_(double(outer) >= std::hypot(double(request.m_width), double(request.m_height))));

        RcResourceEstimate result{};
        u64 previous_bytes{};
        for (u32 index = 0u; index < request.m_cascade_count; ++index) {
            RcCascadeEstimate cascade{};
            cascade.m_spacing_cells = u64{1} << index;
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, multiply_(8u, cascade.m_spacing_cells, cascade.m_directions));
            cascade.m_near_cells = index == 0u ? 0u : cascade.m_spacing_cells / 2u;
            cascade.m_far_cells = cascade.m_spacing_cells;

            const std::array<u64, 2> extent{request.m_width, request.m_height};
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                const u64 last = extent[axis] - 1u;
                cascade.m_nominal_probes[axis] = last / cascade.m_spacing_cells + 1u;
                cascade.m_high_guards[axis] = last % cascade.m_spacing_cells == 0u ? 0u : 1u;
                cascade.m_padded_probes[axis] = cascade.m_nominal_probes[axis] + cascade.m_high_guards[axis];
            }

            u64 probes{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Lighting, multiply_(cascade.m_padded_probes[0], cascade.m_padded_probes[1], probes));
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, multiply_(probes, cascade.m_directions, cascade.m_logical_records));
            cascade.m_resident_records = cascade.m_logical_records;
            if (index == 0u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Lighting, multiply_(
                                  std::min(request.m_c0_tile_width, request.m_width),
                                  std::min(request.m_c0_tile_height, request.m_height), probes));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Lighting, multiply_(probes, cascade.m_directions, cascade.m_resident_records));
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Lighting,
                multiply_(cascade.m_resident_records, request.m_interval_bytes, cascade.m_bytes_per_generation));
            result.m_largest_resource_bytes = std::max(result.m_largest_resource_bytes, cascade.m_bytes_per_generation);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Lighting, add_(
                              result.m_all_levels_bytes_per_generation, cascade.m_bytes_per_generation,
                              result.m_all_levels_bytes_per_generation));

            u64 pair_bytes{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, add_(previous_bytes, cascade.m_bytes_per_generation, pair_bytes));
            result.m_adjacent_live_bytes = std::max(result.m_adjacent_live_bytes, pair_bytes);
            auto &parity_capacity = result.m_parity_buffer_bytes[index % 2u];
            parity_capacity = std::max(parity_capacity, cascade.m_bytes_per_generation);
            previous_bytes = cascade.m_bytes_per_generation;
            result.m_cascades.push_back(cascade);
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting,
            add_(result.m_parity_buffer_bytes[0], result.m_parity_buffer_bytes[1], result.m_streaming_working_bytes));

        // Complete the terminal interval for padded probes too. Twice each
        // farthest axis distance is an exact integer under this fixed phase.
        // Their L1 sum conservatively bounds every Euclidean probe/corner ray,
        // avoiding floating-point underestimation on all supported hosts.
        const auto &top = result.m_cascades.back();
        u64 doubled_terminal{};
        const std::array<u64, 2> world_extent{request.m_width, request.m_height};
        for (std::size_t axis = 0u; axis < 2u; ++axis) {
            u64 last_probe{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Lighting, multiply_(top.m_padded_probes[axis] - 1u, top.m_spacing_cells, last_probe));
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, multiply_(last_probe, 2u, last_probe));
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, add_(last_probe, 1u, last_probe));

            const u64 farthest_axis = std::max(last_probe, 2u * world_extent[axis] - 1u);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, add_(doubled_terminal, farthest_axis, doubled_terminal));
        }

        result.m_terminal_far_cells = std::max(top.m_far_cells, doubled_terminal / 2u + doubled_terminal % 2u);

        u64 cells{};
        u64 optical_bytes{};
        u64 scalar_bytes{};
        u64 generation_bytes{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, multiply_(request.m_width, request.m_height, cells));
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, multiply_(cells, request.m_optical_cell_bytes, optical_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, multiply_(cells, request.m_scalar_cell_bytes, scalar_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(Lighting, add_(optical_bytes, scalar_bytes, generation_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, multiply_(generation_bytes, request.m_live_generations, result.m_peak_bytes));
        const u64 c1_bytes = result.m_cascades.size() > 1u ? result.m_cascades[1].m_bytes_per_generation : 0u;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, multiply_(c1_bytes, request.m_retained_c1_generations, result.m_retained_c1_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, add_(result.m_peak_bytes, result.m_streaming_working_bytes, result.m_peak_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, add_(result.m_peak_bytes, result.m_retained_c1_bytes, result.m_peak_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Lighting, add_(result.m_peak_bytes, request.m_additional_peak_bytes, result.m_peak_bytes));
        result.m_largest_resource_bytes = std::max(
            {result.m_largest_resource_bytes, optical_bytes, scalar_bytes,
                request.m_additional_largest_resource_bytes});

        if (result.m_peak_bytes > request.m_budget_bytes or
            result.m_largest_resource_bytes > request.m_max_resource_bytes) {
            result.m_admission_error = std::make_error_code(std::errc::no_buffer_space);
        }
        return result;
    }
}
