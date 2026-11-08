module;
#include "pP/Macros.h"

module engine.app;

import :renderer.optical_geometry;
import engine.core;
import engine.math;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(OpticalGeometry, debug, none)
}

namespace pP::lighting {
    namespace {
        // ------------------------------------------------------------------
        // rendered quadratic topology and deterministic hit-source lookup
        // ------------------------------------------------------------------

        static_assert(std::numeric_limits<double>::is_iec559);

        using Point = std::array<double, 2>;

        Point add(const Point a, const Point b) {
            return {a[0] + b[0], a[1] + b[1]};
        }

        Point sub(const Point a, const Point b) {
            return {a[0] - b[0], a[1] - b[1]};
        }

        Point scale(const Point a, const double b) {
            return {a[0] * b, a[1] * b};
        }

        double dot(const Point a, const Point b) {
            return a[0] * b[0] + a[1] * b[1];
        }

        double cross(const Point a, const Point b) {
            return a[0] * b[1] - a[1] * b[0];
        }

        Point point(const u32 index) {
            const auto value = math::contourPoints()[index];
            return {double(value.m_x), double(value.m_y)};
        }

        // ------------------------------------------------------------------
        // canonical binary32 coefficient construction
        // ------------------------------------------------------------------

        // Canonical coefficient construction is integer-only, matching the
        // declared binary32 RTE input field rather than host FP environment.
        static_assert(
            sizeof(float) == sizeof(u32) and
            std::numeric_limits<float>::is_iec559 and
            std::numeric_limits<float>::digits == 24 and
            std::numeric_limits<float>::max_exponent == 128);

        bool validRoundnessBits_(const u32 raw) noexcept {
            const u32 magnitude = raw & 0x7fffffffu;
            return magnitude <= 0x3f800000u and ((raw & 0x80000000u) == 0u or magnitude == 0u);
        }

        u32 roundnessShiftJam_(const u32 value, const u32 distance) noexcept {
            if (distance == 0u) {
                return value;
            }

            if (distance >= 32u) {
                return value != 0u ? 1u : 0u;
            }

            return (value >> distance) | ((value & ((1u << distance) - 1u)) != 0u ? 1u : 0u);
        }

        u32 roundnessMeanBits_(const u32 a, const u32 b) noexcept {
            const u32 large = std::max(a & 0x7fffffffu, b & 0x7fffffffu);
            const u32 small = std::min(a & 0x7fffffffu, b & 0x7fffffffu);
            const u32 large_exponent = std::max(large >> 23u, 1u);
            const u32 small_exponent = std::max(small >> 23u, 1u);
            const u32 large_significand = (large & 0x007fffffu) | (large >= 0x00800000u ? 0x00800000u : 0u);
            const u32 small_significand = (small & 0x007fffffu) | (small >= 0x00800000u ? 0x00800000u : 0u);

            u32 sum = (large_significand << 3u) +
                      roundnessShiftJam_(small_significand << 3u, large_exponent - small_exponent);
            u32 exponent = large_exponent - 1u;
            if (sum >= 0x08000000u) {
                sum = roundnessShiftJam_(sum, 1u);
                ++exponent;
            }

            if (exponent == 0u) {
                sum = roundnessShiftJam_(sum, 1u);
            }

            u32 significand = sum >> 3u;
            const u32 remainder = sum & 7u;
            if (remainder > 4u or (remainder == 4u and (significand & 1u) != 0u)) {
                ++significand;
            }

            if (exponent == 0u) {
                return significand;
            }

            if (significand >= 0x01000000u) {
                significand >>= 1u;
                ++exponent;
            }

            return (exponent << 23u) | (significand & 0x007fffffu);
        }

        double roundnessMean_(const float a, const float b) noexcept {
            return double(std::bit_cast<float>(roundnessMeanBits_(std::bit_cast<u32>(a), std::bit_cast<u32>(b))));
        }

        // ------------------------------------------------------------------
        // rendered macro geometry and source ownership
        // ------------------------------------------------------------------

        const OpticalTerrainMaterial &material(const OpticalTerrainView &view, const u32 id) {
            const auto found = std::ranges::find(view.m_materials, id, &OpticalTerrainMaterial::m_material);
            PPR_ASSERT(found != view.m_materials.end());
            return *found;
        }

        std::array<u32, 4> corners(const OpticalTerrainView &view, const i32 x, const i32 y) {
            std::array<u32, 4> result{};
            for (u32 index = 0u; index < 4u; ++index) {
                const auto offset = math::contourPoints()[index];
                const i32 sx = x + static_cast<i32>(offset.m_x), sy = y + static_cast<i32>(offset.m_y);
                result[index] = sx < 0 or
                                        sy < 0 or
                                        sx >= static_cast<i32>(view.m_width) or
                                        sy >= static_cast<i32>(view.m_height)
                                    ? view.m_outside_material
                                    : view.m_labels[static_cast<std::size_t>(sy) * view.m_width + static_cast<u32>(sx)];
            }

            return result;
        }

        u32 occupied(const std::array<u32, 4> &labels) {
            u32 mask{};
            for (u32 corner = 0u; corner < 4u; ++corner) {
                if (labels[corner] != 0u) {
                    mask |= 1u << corner;
                }
            }

            return mask;
        }

        bool connected(const std::array<u32, 4> &labels, const u32 mask) {
            return (mask == 5u and labels[0] == labels[2]) or (mask == 10u and labels[1] == labels[3]);
        }

        Point warp(const Point p, const double roundness) {
            Point result{};
            for (u32 axis = 0u; axis < 2u; ++axis) {
                const double lower = std::min(p[axis], 1.0 - p[axis]);
                const double s = 1.0 + roundness;
                const double mapped = 2.0 * lower / (s + std::sqrt(std::max(s * s - 8.0 * roundness * lower, 0.0)));
                result[axis] = p[axis] <= 0.5 ? mapped : 1.0 - mapped;
            }

            return result;
        }

        bool contains(const u32 mask, const Point p) {
            const auto &shape = math::contourCases()[mask];
            for (u32 index = 0u; index < shape.m_triangle_count; ++index) {
                const auto triangle = shape.m_triangles[index];
                const Point a = point(triangle.m_a), b = point(triangle.m_b), c = point(triangle.m_c);
                if (cross(sub(b, a), sub(p, a)) >= 0.0 and
                    cross(sub(c, b), sub(p, b)) >= 0.0 and
                    cross(sub(a, c), sub(p, c)) >= 0.0) {
                    return true;
                }
            }

            return false;
        }

        struct Arc {
            Point m_a{}, m_control{}, m_b{};
            double m_roundness{};
        };

        Arc arc(const math::ContourSegment segment, const double roundness) {
            const Point a = point(segment.m_begin), b = point(segment.m_end);
            const Point midpoint = scale(add(a, b), 0.5);
            const bool curved = a[0] != b[0] and a[1] != b[1];
            return {a, curved ? add(midpoint, scale(sub({0.5, 0.5}, midpoint), roundness)) : midpoint, b, roundness};
        }

        Point curve(const Arc &value, const double t) {
            return add(
                add(scale(value.m_a, (1.0 - t) * (1.0 - t)), scale(value.m_control, 2.0 * t * (1.0 - t))),
                scale(value.m_b, t * t));
        }

        float endpointRoundness(const OpticalTerrainView &view, const std::array<u32, 4> &labels, const Point p) {
            const u32 corner = p[1] == 0.0 ? 0u : p[0] == 1.0 ? 1u : p[1] == 1.0 ? 2u : 3u;
            return material(view, labels[corner] != 0u ? labels[corner] : labels[(corner + 1u) & 3u]).m_roundness;
        }

        std::vector<Arc> outerArcs(const OpticalTerrainView &view, const std::array<u32, 4> &labels) {
            std::vector<Arc> result{};
            const u32 mask = occupied(labels);
            if (mask == 0u or mask == 15u) {
                return result;
            }

            const bool bridge = connected(labels, mask);
            if ((mask == 5u or mask == 10u) and not bridge) {
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    if (labels[corner] == 0u) {
                        continue;
                    }

                    const auto &shape = math::contourCases()[1u << corner];
                    result.push_back(arc(shape.m_segments[0], double(material(view, labels[corner]).m_roundness)));
                }
            } else {
                const auto &shape = math::contourCases()[bridge ? 15u ^ mask : mask];
                for (u32 index = 0u; index < shape.m_segment_count; ++index) {
                    const auto segment = shape.m_segments[index];
                    const double roundness = roundnessMean_(
                        endpointRoundness(view, labels, point(segment.m_begin)),
                        endpointRoundness(view, labels, point(segment.m_end)));
                    result.push_back(arc(segment, roundness));
                }
            }

            return result;
        }

        std::expected<bool, std::error_code> cornerInside(Point p, const u32 corner, const double roundness) {
            if (corner == 1u or corner == 2u) {
                p[0] = 1.0 - p[0];
            }

            if (corner == 2u or corner == 3u) {
                p[1] = 1.0 - p[1];
            }

            const double difference = p[0] - p[1];
            const double field = p[0] + p[1] - 0.5 - 0.25 * roundness + roundness * difference * difference;
            constexpr double unit = std::numeric_limits<double>::epsilon() * 0.5;
            constexpr double gamma32 = 32.0 * unit / (1.0 - 32.0 * unit);
            const double magnitude = std::abs(p[0]) + std::abs(p[1]);
            const double uncertainty = gamma32 *
                                       (magnitude + 0.5 + 0.25 * roundness + roundness * magnitude * magnitude);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, not std::isfinite(field) or std::abs(field) <= uncertainty
                                     ? std::make_error_code(std::errc::argument_out_of_domain)
                                     : std::error_code{});
            return field < 0.0;
        }

        std::expected<bool, std::error_code> inside(
            const OpticalTerrainView &view, const std::array<u32, 4> &labels, const Point p) {
            const u32 mask = occupied(labels);
            if (mask == 0u or mask == 15u) {
                return mask == 15u;
            }

            if (mask == 3u or mask == 6u or mask == 9u or mask == 12u) {
                const double coordinate = mask == 6u or mask == 9u ? p[0] : p[1];
                const double uncertainty = 8.0 * std::numeric_limits<double>::epsilon() * (1.0 + std::abs(coordinate));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    OpticalGeometry, std::abs(coordinate - 0.5) <= uncertainty
                                         ? std::make_error_code(std::errc::argument_out_of_domain)
                                         : std::error_code{});
                return mask == 6u or mask == 12u ? coordinate > 0.5 : coordinate < 0.5;
            }

            const bool bridge = connected(labels, mask);
            if (mask == 5u or mask == 10u) {
                const u32 selected = bridge ? 15u ^ mask : mask;
                const double bridge_roundness = bridge
                                                    ? double(material(view, labels[mask == 5u ? 0u : 1u]).m_roundness)
                                                    : 0.0;
                bool any_inside = false;
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    if ((selected & (1u << corner)) == 0u) {
                        continue;
                    }

                    const auto in_corner = cornerInside(
                        p, corner, bridge ? bridge_roundness : double(material(view, labels[corner]).m_roundness));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(OpticalGeometry, in_corner ? std::error_code{} : in_corner.error());
                    any_inside = any_inside or *in_corner;
                }

                return bridge ? not any_inside : any_inside;
            }

            const bool complement = mask == 7u or mask == 11u or mask == 13u or mask == 14u;
            const u32 corner_mask = complement ? 15u ^ mask : mask;
            u32 corner{};
            while ((corner_mask & (1u << corner)) == 0u) {
                ++corner;
            }

            const auto arcs = outerArcs(view, labels);
            const auto in_corner = cornerInside(p, corner, arcs.front().m_roundness);
            PPR_RETURN_UNEXPECTED_ON_FAIL(OpticalGeometry, in_corner ? std::error_code{} : in_corner.error());
            return complement ? not *in_corner : *in_corner;
        }

        // Match the declared render hit-source query's bounded closest-point
        // algorithm. This is not a claim of an exact signed-distance solver.
        double distance(const Point p, const Arc &value) {
            const Point delta = sub(value.m_b, value.m_a);
            double t = std::clamp(dot(sub(p, value.m_a), delta) / dot(delta, delta), 0.0, 1.0);
            Point nearest = add(value.m_a, scale(delta, t));
            if (value.m_roundness != 0.0 and delta[0] != 0.0 and delta[1] != 0.0) {
                const Point acceleration = scale(add(sub(value.m_a, scale(value.m_control, 2.0)), value.m_b), 2.0);
                double best = std::min(
                    dot(sub(p, value.m_a), sub(p, value.m_a)), dot(sub(p, value.m_b), sub(p, value.m_b)));
                double best_t = dot(sub(p, value.m_a), sub(p, value.m_a)) < dot(sub(p, value.m_b), sub(p, value.m_b))
                                    ? 0.0
                                    : 1.0;
                for (u32 seed = 0u; seed < 3u; ++seed) {
                    t = 0.5 * seed;
                    for (u32 iteration = 0u; iteration < 6u; ++iteration) {
                        const Point q = curve(value, t);
                        const Point tangent = scale(
                            add(scale(sub(value.m_control, value.m_a), 1.0 - t),
                                scale(sub(value.m_b, value.m_control), t)),
                            2.0);
                        const double numerator = dot(sub(q, p), tangent);
                        const double denominator = dot(tangent, tangent) + dot(sub(q, p), acceleration);
                        if (std::abs(denominator) > 1e-8) {
                            t = std::clamp(t - std::clamp(numerator / denominator, -0.25, 0.25), 0.0, 1.0);
                        }
                    }

                    const Point delta_p = sub(p, curve(value, t));
                    const double candidate = dot(delta_p, delta_p);
                    if (candidate < best) {
                        best = candidate;
                        best_t = t;
                    }
                }

                nearest = curve(value, best_t);
            }

            const Point delta_p = sub(p, nearest);
            return std::sqrt(dot(delta_p, delta_p));
        }

        u32 sourceLabel(const OpticalTerrainView &view, const std::array<u32, 4> &labels, const Point p) {
            if (std::ranges::all_of(labels, [&](const u32 id) { return id == labels[0]; })) {
                return labels[0];
            }

            std::optional<double> shared{};
            if (occupied(labels) == 15u) {
                const auto second = std::ranges::find_if(labels, [&](const u32 id) { return id != labels[0]; });
                if (second != labels.end() and
                    std::ranges::all_of(labels, [&](const u32 id) { return id == labels[0] or id == *second; })) {
                    shared = roundnessMean_(material(view, labels[0]).m_roundness, material(view, *second).m_roundness);
                }
            }

            u32 visited{}, winner{};
            double best = -std::numeric_limits<double>::infinity();
            for (u32 corner = 0u; corner < 4u; ++corner) {
                const u32 id = labels[corner];
                if (id == 0u or (visited & (1u << corner)) != 0u) {
                    continue;
                }

                u32 mask{};
                for (u32 other = 0u; other < 4u; ++other) {
                    if (labels[other] == id) {
                        mask |= 1u << other;
                    }
                }

                visited |= mask;
                const u32 other = labels[(corner + 1u) & 3u], opposite = labels[(corner + 3u) & 3u];
                const bool bridge = (mask == 5u or mask == 10u) and (other != opposite or other == 0u or id < other);
                const u32 shape_mask = bridge ? 15u ^ mask : mask;
                const double roundness = shared.value_or(double(material(view, id).m_roundness));
                const auto &shape = math::contourCases()[shape_mask];

                double nearest = std::numeric_limits<double>::infinity();
                for (u32 segment = 0u; segment < shape.m_segment_count; ++segment) {
                    nearest = std::min(nearest, distance(p, arc(shape.m_segments[segment], roundness)));
                }

                const double score = (contains(shape_mask, warp(p, roundness)) != bridge) ? nearest : -nearest;
                if (winner == 0u or score > best) {
                    best = score;
                    winner = id;
                }
            }

            return winner;
        }

        // ------------------------------------------------------------------
        // independent slab and quadratic-event reference
        // ------------------------------------------------------------------

        bool clip(
            const Point origin, const Point direction, const Point low, const Point high, double &near, double &far,
            bool &uncertain) {
            for (u32 axis = 0u; axis < 2u; ++axis) {
                if (direction[axis] == 0.0) {
                    if (origin[axis] < low[axis] or origin[axis] >= high[axis]) {
                        return false;
                    }

                    continue;
                }

                const double first = (low[axis] - origin[axis]) / direction[axis],
                             last = (high[axis] - origin[axis]) / direction[axis];
                if (not std::isfinite(first) or not std::isfinite(last) or first == last) {
                    uncertain = true;
                    return false;
                }

                near = std::max(near, std::min(first, last));
                far = std::min(far, std::max(first, last));
            }

            return near < far;
        }

        std::error_code events(
            const Arc &value, const Point origin, const Point direction, const double near, const double far,
            std::vector<double> &output) {
            std::array<double, 2> roots{};
            u32 count{};

            // For a constant ray coordinate at a strict endpoint extremum,
            // Bernstein weights prove that the curve reaches that coordinate
            // only at that endpoint. This identifies its complete event support
            // without interpreting a rounded zero discriminant as a tangent.
            for (u32 axis = 0u; axis < 2u; ++axis) {
                if (direction[axis] != 0.0 or value.m_a[axis] == value.m_b[axis]) {
                    continue;
                }

                const double low = std::min(value.m_a[axis], value.m_b[axis]);
                const double high = std::max(value.m_a[axis], value.m_b[axis]);
                if (value.m_control[axis] < low or value.m_control[axis] > high) {
                    continue;
                }

                if (origin[axis] == value.m_a[axis]) {
                    roots[count++] = 0.0;
                    break;
                }

                if (origin[axis] == value.m_b[axis]) {
                    roots[count++] = 1.0;
                    break;
                }
            }

            if (count == 0u) {
                const Point a = add(sub(value.m_a, scale(value.m_control, 2.0)), value.m_b);
                const Point b = scale(sub(value.m_control, value.m_a), 2.0);
                const double qa = cross(a, direction), qb = cross(b, direction),
                             qc = cross(sub(value.m_a, origin), direction);

                // A bounded rounding-error certificate rejects near-degenerate
                // double events as unavailable reference evidence. The budget is
                // gamma64 times the absolute coefficient-expression sum, rather
                // than a spatial epsilon or permission to merge roots.
                constexpr double unit = std::numeric_limits<double>::epsilon() * 0.5;
                constexpr double gamma64 = 64.0 * unit / (1.0 - 64.0 * unit);
                const double aa = std::abs(a[0] * direction[1]) + std::abs(a[1] * direction[0]);
                const double bb = std::abs(b[0] * direction[1]) + std::abs(b[1] * direction[0]);
                const double cc = (std::abs(value.m_a[0]) + std::abs(origin[0])) * std::abs(direction[1]) +
                                  (std::abs(value.m_a[1]) + std::abs(origin[1])) * std::abs(direction[0]);
                const bool equal_curvature = value.m_a[0] + value.m_b[0] == value.m_a[1] + value.m_b[1];
                const bool linear = value.m_roundness == 0.0 or
                                    value.m_a[0] == value.m_b[0] or
                                    value.m_a[1] == value.m_b[1] or
                                    (equal_curvature and direction[0] == direction[1]) or
                                    (not equal_curvature and direction[0] == -direction[1]);
                PPR_RETURN_ERROR_ON_FAIL(
                    OpticalGeometry, not std::isfinite(qa) or
                                             not std::isfinite(qb) or
                                             not std::isfinite(qc) or
                                             (not linear and std::abs(qa) <= gamma64 * aa)
                                         ? std::make_error_code(std::errc::argument_out_of_domain)
                                         : std::error_code{});

                if (linear) {
                    const bool parallel = (b[0] == 0.0 and direction[0] == 0.0) or
                                          (b[1] == 0.0 and direction[1] == 0.0) or
                                          (b[0] == b[1] and direction[0] == direction[1]) or
                                          (b[0] == -b[1] and direction[0] == -direction[1]);
                    PPR_RETURN_ERROR_ON_FAIL(
                        OpticalGeometry, qb == 0.0 and not parallel
                                             ? std::make_error_code(std::errc::argument_out_of_domain)
                                             : std::error_code{});

                    if (qb != 0.0) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            OpticalGeometry, std::abs(qb) <= gamma64 * bb
                                                 ? std::make_error_code(std::errc::argument_out_of_domain)
                                                 : std::error_code{});
                        roots[count++] = -qc / qb;
                    }
                } else {
                    const double discriminant = qb * qb - 4.0 * qa * qc;
                    const double uncertainty = gamma64 * (bb * bb + 4.0 * aa * cc);
                    PPR_RETURN_ERROR_ON_FAIL(
                        OpticalGeometry, not std::isfinite(discriminant) or
                                                 not std::isfinite(uncertainty) or
                                                 std::abs(discriminant) <= uncertainty
                                             ? std::make_error_code(std::errc::argument_out_of_domain)
                                             : std::error_code{});

                    if (discriminant > 0.0) {
                        const double q = -0.5 * (qb + std::copysign(std::sqrt(discriminant), qb));
                        if (q == 0.0) {
                            roots[count++] = -qb / (2.0 * qa);
                        } else {
                            roots[count++] = q / qa;
                            roots[count++] = qc / q;
                        }
                    }
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                OpticalGeometry, count == 2u and roots[0] == roots[1]
                                     ? std::make_error_code(std::errc::argument_out_of_domain)
                                     : std::error_code{});

            const u32 axis = std::abs(direction[0]) >= std::abs(direction[1]) ? 0u : 1u;
            for (u32 index = 0u; index < count; ++index) {
                const double u = roots[index];
                PPR_RETURN_ERROR_ON_FAIL(
                    OpticalGeometry,
                    not std::isfinite(u) ? std::make_error_code(std::errc::argument_out_of_domain) : std::error_code{});

                if (u < 0.0 or u > 1.0) {
                    continue;
                }

                const double t = (curve(value, u)[axis] - origin[axis]) / direction[axis];
                PPR_RETURN_ERROR_ON_FAIL(
                    OpticalGeometry,
                    not std::isfinite(t) ? std::make_error_code(std::errc::argument_out_of_domain) : std::error_code{});

                if (t > near and t < far) {
                    output.push_back(t);
                }
            }

            return {};
        }
    }

    // ------------------------------------------------------------------
    // checked canonical coefficients and immutable capture
    // ------------------------------------------------------------------

    std::expected<float, std::error_code> canonicalTerrainRoundnessMean(const float a, const float b) {
        const u32 first = std::bit_cast<u32>(a), second = std::bit_cast<u32>(b);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry, not validRoundnessBits_(first) or not validRoundnessBits_(second)
                                 ? std::make_error_code(std::errc::invalid_argument)
                                 : std::error_code{});

        return std::bit_cast<float>(roundnessMeanBits_(first, second));
    }

    std::expected<OpticalTerrainSnapshot, std::error_code> OpticalTerrainSnapshot::capture(
        const OpticalTerrainView &input) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry, input.m_boundary != OpticalBoundary::vacuum and input.m_boundary != OpticalBoundary::opaque
                                 ? std::make_error_code(std::errc::invalid_argument)
                                 : std::error_code{});
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry,
            input.m_width == 0u or input.m_height == 0u or u64{input.m_width} * input.m_height != input.m_labels.size()
                ? std::make_error_code(std::errc::invalid_argument)
                : std::error_code{});
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry, input.m_labels.size() > kMaxQualificationSamples
                                 ? std::make_error_code(std::errc::not_supported)
                                 : std::error_code{});

        std::vector<OpticalCell> validation(input.m_labels.size());
        for (auto &cell: validation) {
            cell.m_kind = OpticalKind::vacuum;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry, validateOpticalGrid(
                                 {validation, input.m_min_m, input.m_cell_size_m, input.m_width, input.m_height,
                                     OpticalBoundary::vacuum}));

        // Every sample-centre and midpoint plane must survive world anchoring.
        // Integer cell endpoints alone do not certify the dual-grid half planes.
        for (u32 axis = 0u; axis < 2u; ++axis) {
            const u32 extent = axis == 0u ? input.m_width : input.m_height;
            double previous = input.m_min_m[axis];
            for (u32 plane = 1u; plane <= 2u * extent; ++plane) {
                const double next = input.m_min_m[axis] + 0.5 * plane * input.m_cell_size_m;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    OpticalGeometry, not std::isfinite(next) or not (next > previous)
                                         ? std::make_error_code(std::errc::invalid_argument)
                                         : std::error_code{});
                previous = next;
            }
        }

        std::array<bool, 65536> seen{};
        for (const auto &row: input.m_materials) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, row.m_material >= 65535u or
                                         seen[row.m_material] or
                                         not std::isfinite(row.m_roundness) or
                                         row.m_roundness < 0.0f or
                                         row.m_roundness > 1.0f
                                     ? std::make_error_code(std::errc::invalid_argument)
                                     : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry,
                row.m_optics.m_kind != OpticalKind::vacuum and row.m_optics.m_kind != OpticalKind::opaque
                    ? std::make_error_code(std::errc::not_supported)
                    : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, (row.m_material == 0u) != (row.m_optics.m_kind == OpticalKind::vacuum)
                                     ? std::make_error_code(std::errc::invalid_argument)
                                     : std::error_code{});

            const std::span<const OpticalCell> single{&row.m_optics, 1u};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, validateOpticalGrid({single, {}, 1.0, 1u, 1u, OpticalBoundary::vacuum}));

            seen[row.m_material] = true;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry, input.m_outside_material >= 65535u or not seen[0] or not seen[input.m_outside_material]
                                 ? std::make_error_code(std::errc::no_message)
                                 : std::error_code{});

        for (const u16 id: input.m_labels) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, not seen[id] ? std::make_error_code(std::errc::no_message) : std::error_code{});
        }

        OpticalTerrainSnapshot result{};
        result.m_materials.assign(input.m_materials.begin(), input.m_materials.end());
        result.m_labels.assign(input.m_labels.begin(), input.m_labels.end());
        result.m_min_m = input.m_min_m;
        result.m_cell_size_m = input.m_cell_size_m;
        result.m_width = input.m_width;
        result.m_height = input.m_height;
        result.m_outside_material = input.m_outside_material;
        result.m_boundary = input.m_boundary;
        return result;
    }

    OpticalTerrainView OpticalTerrainSnapshot::view() const noexcept {
        return {m_labels, m_materials, m_min_m, m_cell_size_m, m_width, m_height, m_outside_material, m_boundary};
    }

    // ------------------------------------------------------------------
    // complete small-domain double optical oracle
    // ------------------------------------------------------------------

    namespace {
        std::expected<double, std::error_code> physicalHitDistance_(
            const double parameter, const double metres_per_lambda) {
            const double distance = parameter * metres_per_lambda;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, not std::isfinite(distance) or distance < 0.0 or (parameter > 0.0 and distance == 0.0)
                                     ? std::make_error_code(std::errc::result_out_of_range)
                                     : std::error_code{});
            return distance;
        }

        std::expected<OpticalTerrainTrace, std::error_code> traceTerrainParametric_(
            const OpticalTerrainSnapshot &snapshot, const Point origin, const Point direction,
            const double requested_near, const double requested_far, const double metres_per_lambda) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, not std::isfinite(requested_near) or
                                         not std::isfinite(requested_far) or
                                         requested_near < 0.0 or
                                         requested_far < requested_near or
                                         not std::isfinite(metres_per_lambda) or
                                         metres_per_lambda <= 0.0
                                     ? std::make_error_code(std::errc::invalid_argument)
                                     : std::error_code{});

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, not std::isfinite(origin[0]) or not std::isfinite(origin[1])
                                     ? std::make_error_code(std::errc::result_out_of_range)
                                     : std::error_code{});

            const auto view = snapshot.view();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry, view.m_labels.empty() or
                                         view.m_materials.empty() or
                                         u64{view.m_width} * view.m_height != view.m_labels.size()
                                     ? std::make_error_code(std::errc::invalid_argument)
                                     : std::error_code{});

            OpticalTerrainTrace result{};
            if (requested_near == requested_far) {
                return result;
            }

            for (const double component: direction) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    OpticalGeometry, component != 0.0 and std::abs(component) < std::numeric_limits<double>::min()
                                         ? std::make_error_code(std::errc::argument_out_of_domain)
                                         : std::error_code{});
            }

            double world_near = requested_near, world_far = requested_far;
            bool uncertain = false;
            const bool intersects = clip(
                origin, direction, {}, {double(view.m_width), double(view.m_height)}, world_near, world_far, uncertain);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                OpticalGeometry,
                uncertain ? std::make_error_code(std::errc::argument_out_of_domain) : std::error_code{});

            auto hit = [&](const u32 id, const double t) -> std::error_code {
                const auto distance = physicalHitDistance_(t, metres_per_lambda);
                PPR_RETURN_ERROR_ON_FAIL(OpticalGeometry, distance ? std::error_code{} : distance.error());

                result.m_material = id;
                result.m_hit_m = *distance;
                result.m_interval.m_radiance = material(view, id).m_optics.m_surface_emission;
                result.m_interval.m_transmittance = {};
                return {};
            };

            if (view.m_boundary == OpticalBoundary::opaque and (not intersects or world_near > requested_near)) {
                const auto distance = physicalHitDistance_(requested_near, metres_per_lambda);
                PPR_RETURN_UNEXPECTED_ON_FAIL(OpticalGeometry, distance ? std::error_code{} : distance.error());

                result.m_hit_m = *distance;
                result.m_interval.m_transmittance = {};
                return result;
            }

            double earliest = requested_far;
            if (intersects) {
                for (i32 y = -1; y < static_cast<i32>(view.m_height); ++y) {
                    for (i32 x = -1; x < static_cast<i32>(view.m_width); ++x) {
                        const Point low{double(x) + 0.5, double(y) + 0.5};
                        double near = world_near, far = std::min(world_far, earliest);
                        const bool intersects_cell = clip(
                            origin, direction, low, add(low, {1.0, 1.0}), near, far, uncertain);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            OpticalGeometry,
                            uncertain ? std::make_error_code(std::errc::argument_out_of_domain) : std::error_code{});
                        if (not intersects_cell) {
                            continue;
                        }

                        ++result.m_dual_cells;

                        const auto labels = corners(view, x, y);
                        const Point local_origin = sub(origin, low);
                        std::vector<double> cuts{near, far};
                        const auto boundaries = outerArcs(view, labels);
                        for (u32 index = 0u; index < boundaries.size(); ++index) {
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                OpticalGeometry, events(boundaries[index], local_origin, direction, near, far, cuts));
                        }

                        std::ranges::sort(cuts);
                        const auto duplicates = std::ranges::unique(cuts);
                        cuts.erase(duplicates.begin(), duplicates.end());
                        for (std::size_t index = 1u; index < cuts.size(); ++index) {
                            const double middle = std::midpoint(cuts[index - 1u], cuts[index]);
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                OpticalGeometry, not (middle > cuts[index - 1u] and middle < cuts[index])
                                                     ? std::make_error_code(std::errc::result_out_of_range)
                                                     : std::error_code{});

                            const auto occupied_interval = inside(
                                view, labels, add(local_origin, scale(direction, middle)));
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                OpticalGeometry, occupied_interval ? std::error_code{} : occupied_interval.error());

                            if (not *occupied_interval) {
                                continue;
                            }

                            const Point start = add(local_origin, scale(direction, cuts[index - 1u]));
                            const u32 id = sourceLabel(view, labels, start);
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                OpticalGeometry,
                                id == 0u ? std::make_error_code(std::errc::state_not_recoverable) : std::error_code{});

                            earliest = cuts[index - 1u];
                            PPR_RETURN_UNEXPECTED_ON_FAIL(OpticalGeometry, hit(id, earliest));
                            break;
                        }
                    }
                }
            }

            if (not result.m_hit_m and
                view.m_boundary == OpticalBoundary::opaque and
                intersects and
                world_far < requested_far) {
                const auto distance = physicalHitDistance_(world_far, metres_per_lambda);
                PPR_RETURN_UNEXPECTED_ON_FAIL(OpticalGeometry, distance ? std::error_code{} : distance.error());

                result.m_hit_m = *distance;
                result.m_interval.m_transmittance = {};
            }

            return result;
        }
    }

    std::expected<OpticalTerrainTrace, std::error_code> traceOpticalTerrainParametricReference(
        const OpticalTerrainSnapshot &snapshot, const OpticalTerrainParametricRay &ray) {
        const double length = std::hypot(ray.m_direction[0], ray.m_direction[1]);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry, not std::isfinite(length) or length <= 0.0
                                 ? std::make_error_code(std::errc::invalid_argument)
                                 : std::error_code{});

        return traceTerrainParametric_(
            snapshot, ray.m_origin_cells, ray.m_direction, ray.m_near_lambda, ray.m_far_lambda,
            length * snapshot.view().m_cell_size_m);
    }

    std::expected<OpticalTerrainTrace, std::error_code> traceOpticalTerrainConnectorReference(
        const OpticalTerrainSnapshot &snapshot, const Point start_cells, const Point end_cells) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            OpticalGeometry, snapshot.view().m_labels.empty() or
                                     not std::isfinite(start_cells[0]) or
                                     not std::isfinite(start_cells[1]) or
                                     not std::isfinite(end_cells[0]) or
                                     not std::isfinite(end_cells[1])
                                 ? std::make_error_code(std::errc::invalid_argument)
                                 : std::error_code{});

        if (start_cells == end_cells) {
            return OpticalTerrainTrace{};
        }

        return traceOpticalTerrainParametricReference(snapshot, {start_cells, sub(end_cells, start_cells), 0.0, 1.0});
    }

    std::expected<OpticalTerrainTrace, std::error_code> traceOpticalTerrainReference(
        const OpticalTerrainSnapshot &snapshot, const OpticalRay &ray) {
        const auto normalized = normalizeOpticalRay(ray);
        PPR_RETURN_UNEXPECTED_ON_FAIL(OpticalGeometry, normalized ? std::error_code{} : normalized.error());

        const auto view = snapshot.view();
        if (normalized->m_near_m == normalized->m_far_m) {
            return traceTerrainParametric_(snapshot, {}, normalized->m_direction, 0.0, 0.0, view.m_cell_size_m);
        }

        const Point origin{
            (normalized->m_origin_m[0] - view.m_min_m[0]) / view.m_cell_size_m,
            (normalized->m_origin_m[1] - view.m_min_m[1]) / view.m_cell_size_m};
        return traceTerrainParametric_(
            snapshot, origin, normalized->m_direction, normalized->m_near_m / view.m_cell_size_m,
            normalized->m_far_m / view.m_cell_size_m, view.m_cell_size_m);
    }
}
