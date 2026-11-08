module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // optical reference fixtures
    // ------------------------------------------------------------------

    namespace LightingTests {
        using namespace lighting;

        void assertInterval_(
            const RadianceInterval &actual, const OpticalRgb &radiance, const OpticalRgb &transmission) {
            for (std::size_t channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::abs(actual.m_radiance[channel] - radiance[channel]) < 1e-11);
                PPR_TEST_ASSERT(std::abs(actual.m_transmittance[channel] - transmission[channel]) < 1e-11);
            }
        }

        OpticalCell vacuum_() {
            OpticalCell cell{};
            cell.m_kind = OpticalKind::vacuum;
            return cell;
        }

        OpticalCell surface_(const OpticalRgb emission) {
            OpticalCell cell{};
            cell.m_surface_emission = emission;
            cell.m_kind = OpticalKind::opaque;
            return cell;
        }

        // ------------------------------------------------------------------
        // canonical rendered coefficient and independent Bezier consumption
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (contour_roundness_mean_is_explicit_binary32_rte) {
            // Frozen dyadic expectations, not a second implementation of the
            // production significand alignment or native host FP rounding.
            constexpr std::array<std::array<u32, 3>, 17> cases{{
                {0x3f0ccccdu, 0x3f19999au, 0x3f133334u}, // .55f/.6f, tie to even.
                {0x3f19999au, 0x3f0ccccdu, 0x3f133334u},
                {0u, 0x3f800000u, 0x3f000000u},
                {0x3f800000u, 0x3f800000u, 0x3f800000u},
                {0u, 0u, 0u},
                {0x3f000000u, 0x3f000001u, 0x3f000000u},
                {0x3f000001u, 0x3f000002u, 0x3f000002u},
                {0x3effffffu, 0x3f000000u, 0x3f000000u}, // Binade carry.
                {0x3f7fffffu, 0x3f800000u, 0x3f800000u},
                {0x3f800000u, 0x2b800000u, 0x3f000000u}, // Shift40, sticky only.
                {0x3f800000u, 0x33800000u, 0x3f000000u},
                {0x3f800000u, 0x33800001u, 0x3f000001u}, // Just above midpoint.
                {0u, 1u, 0u},
                {0u, 3u, 2u},
                {0x007fffffu, 0x00800000u, 0x00800000u},
                {0x00800000u, 0u, 0x00400000u},
                {0x80000000u, 0u, 0u}, // Canonical positive zero.
            }};

            for (const auto &item: cases) {
                const auto mean = canonicalTerrainRoundnessMean(
                    std::bit_cast<float>(item[0]), std::bit_cast<float>(item[1]));
                PPR_TEST_ASSERT(mean.has_value() and std::bit_cast<u32>(*mean) == item[2]);
            }

            for (const u32 invalid: {0xbf000000u, 0x3f800001u, 0x7f800000u, 0x7fc00000u}) {
                const auto result = canonicalTerrainRoundnessMean(std::bit_cast<float>(invalid), 0.5f);
                PPR_TEST_ASSERT(not result and result.error() == std::errc::invalid_argument);
            }

            const std::array<u16, 4> labels{17u, 23u, 0u, 23u}; // Row-major: top-left vacuum (clockwise mask7).
            const std::array<OpticalTerrainMaterial, 3> materials{
                OpticalTerrainMaterial{vacuum_(), 0u, 0.0f},
                {surface_({8.0, 2.0, 1.0}), 17u, std::bit_cast<float>(0x3f0ccccdu)},
                {surface_({8.0, 2.0, 1.0}), 23u, std::bit_cast<float>(0x3f19999au)}};
            const auto shape = OpticalTerrainSnapshot::capture(
                {labels, materials, {}, 1.0, 2u, 2u, 0u, OpticalBoundary::vacuum});
            PPR_TEST_ASSERT(shape.has_value());
            PPR_TEST_ASSERT(std::bit_cast<u32>(shape->view().m_materials[1].m_roundness) == 0x3f0ccccdu);
            PPR_TEST_ASSERT(std::bit_cast<u32>(shape->view().m_materials[2].m_roundness) == 0x3f19999au);

            // The normal through this independent Bezier midpoint hits at the
            // canonical promoted coefficient, not the unrounded double mean.
            const auto hit = traceOpticalTerrainReference(*shape, {{0.5, 1.5}, {1.0, -1.0}, 0.0, 1.0});
            const double expected = std::sqrt(2.0) * (0.25 + 0.125 * double(std::bit_cast<float>(0x3f133334u)));
            PPR_TEST_ASSERT(hit.has_value() and hit->m_hit_m.has_value());
            PPR_TEST_ASSERT(std::abs(*hit->m_hit_m - expected) < 1e-13);
            assertInterval_(hit->m_interval, {8.0, 2.0, 1.0}, {});
        };

        // ------------------------------------------------------------------
        // optical interval and transport contracts
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (ordered_intervals_preserve_rgb_transmission_and_identity) {
            const RadianceInterval near{{1.0, 2.0, 3.0}, {0.5, 0.25, 1.0}};
            const RadianceInterval far{{8.0, 12.0, 4.0}, {0.1, 0.2, 0.3}};
            const auto result = composeIntervals(near, far);
            PPR_TEST_ASSERT(result.has_value());
            assertInterval_(*result, {5.0, 5.0, 7.0}, {0.05, 0.05, 0.3});

            const auto left_identity = composeIntervals({}, near);
            const auto right_identity = composeIntervals(near, {});
            PPR_TEST_ASSERT(left_identity.has_value() and right_identity.has_value());
            assertInterval_(*left_identity, near.m_radiance, near.m_transmittance);
            assertInterval_(*right_identity, near.m_radiance, near.m_transmittance);

            const auto reversed = composeIntervals(far, near);
            PPR_TEST_ASSERT(reversed.has_value());
            assertInterval_(*reversed, {8.1, 12.4, 4.9}, {0.05, 0.05, 0.3});
        };

        PPR_UNIT_TEST (analytic_absorbing_slabs_use_order_and_metre_lengths) {
            const double log2 = std::log(2.0);
            const double log4 = std::log(4.0);
            const std::array<OpticalCell, 2> cells{
                OpticalCell{{log2, log4, 0.0}, {log2, 2.0 * log4, 3.0}, {}, OpticalKind::absorbing},
                OpticalCell{{log4, log2, log2}, {4.0 * log4, 6.0 * log2, 2.0 * log2}, {}, OpticalKind::absorbing}};
            const OpticalGridView grid{cells, {0.0, 0.0}, 1.0, 2u, 1u, OpticalBoundary::vacuum};
            const auto forward = traceOpticalReference(grid, {{0.0, 0.5}, {2.0, 0.0}, 0.0, 2.0});
            PPR_TEST_ASSERT(forward.has_value());
            assertInterval_(*forward, {2.0, 2.25, 4.0}, {0.125, 0.125, 0.5});

            const auto reverse = traceOpticalReference(grid, {{2.0, 0.5}, {-3.0, 0.0}, 0.0, 2.0});
            PPR_TEST_ASSERT(reverse.has_value());
            assertInterval_(*reverse, {3.125, 3.75, 2.5}, {0.125, 0.125, 0.5});

            const std::array<OpticalCell, 1> scaled_cells{OpticalCell{
                {4.0 * log2, 4.0 * log2, 4.0 * log2}, {4.0 * log2, 4.0 * log2, 4.0 * log2}, {},
                OpticalKind::absorbing}};
            const OpticalGridView scaled{scaled_cells, {-10.0, 4.0}, 0.25, 1u, 1u, OpticalBoundary::vacuum};
            const auto metric = traceOpticalReference(scaled, {{-10.0, 4.125}, {7.0, 0.0}, 0.0, 0.25});
            PPR_TEST_ASSERT(metric.has_value());
            assertInterval_(*metric, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5});
        };

        PPR_UNIT_TEST (opaque_emission_is_visible_before_stop_and_hides_far_sources) {
            const double log2 = std::log(2.0);
            const std::array<OpticalCell, 3> cells{
                OpticalCell{{log2, log2, log2}, {}, {}, OpticalKind::absorbing}, surface_({8.0, 12.0, 4.0}),
                surface_({1000.0, 1000.0, 1000.0})};
            const OpticalGridView grid{cells, {0.0, 0.0}, 1.0, 3u, 1u, OpticalBoundary::vacuum};
            const auto result = traceOpticalReference(grid, {{0.0, 0.5}, {1.0, 0.0}, 0.0, 3.0});
            PPR_TEST_ASSERT(result.has_value());
            assertInterval_(*result, {4.0, 6.0, 2.0}, {});

            const auto starts_inside = traceOpticalReference(grid, {{1.5, 0.5}, {1.0, 0.0}, 0.0, 1.0});
            PPR_TEST_ASSERT(starts_inside.has_value());
            assertInterval_(*starts_inside, {8.0, 12.0, 4.0}, {});
        };

        PPR_UNIT_TEST (corner_and_axis_ties_have_one_half_open_owner) {
            OpticalCell emitter{};
            emitter.m_kind = OpticalKind::absorbing;
            emitter.m_volume_emission = {1.0, 2.0, 3.0};
            const std::array<OpticalCell, 4> diagonal{emitter, surface_({}), surface_({}), emitter};
            const OpticalGridView grid{diagonal, {0.0, 0.0}, 1.0, 2u, 2u, OpticalBoundary::vacuum};
            const double diagonal_length = 2.0 * std::sqrt(2.0);
            const auto corner = traceOpticalReference(grid, {{-1.0, -1.0}, {1.0, 1.0}, 0.0, 4.0 * std::sqrt(2.0)});
            PPR_TEST_ASSERT(corner.has_value());
            assertInterval_(*corner, {diagonal_length, 2.0 * diagonal_length, 3.0 * diagonal_length}, {1.0, 1.0, 1.0});

            const std::array<OpticalCell, 4> rows{surface_({}), surface_({}), emitter, emitter};
            const OpticalGridView row_grid{rows, {0.0, 0.0}, 1.0, 2u, 2u, OpticalBoundary::vacuum};
            const auto axis = traceOpticalReference(row_grid, {{2.0, 1.0}, {-1.0, 0.0}, 0.0, 2.0});
            PPR_TEST_ASSERT(axis.has_value());
            assertInterval_(*axis, {2.0, 4.0, 6.0}, {1.0, 1.0, 1.0});

            const auto exterior_edge = traceOpticalReference(row_grid, {{2.0, 2.0}, {-1.0, 0.0}, 0.0, 2.0});
            PPR_TEST_ASSERT(exterior_edge.has_value());
            assertInterval_(*exterior_edge, {}, {1.0, 1.0, 1.0});
        };

        PPR_UNIT_TEST (explicit_world_boundary_and_finite_intervals) {
            const std::array<OpticalCell, 1> cells{vacuum_()};
            OpticalGridView grid{cells, {0.0, 0.0}, 1.0, 1u, 1u, OpticalBoundary::vacuum};
            const OpticalRay ray{{-1.0, 0.5}, {1.0, 0.0}, 0.0, 3.0};
            const auto vacuum = traceOpticalReference(grid, ray);
            PPR_TEST_ASSERT(vacuum.has_value());
            assertInterval_(*vacuum, {}, {1.0, 1.0, 1.0});

            grid.m_boundary = OpticalBoundary::opaque;
            const auto exterior = traceOpticalReference(grid, ray);
            const auto exit = traceOpticalReference(grid, {{0.5, 0.5}, {1.0, 0.0}, 0.0, 2.0});
            PPR_TEST_ASSERT(exterior.has_value() and exit.has_value());
            assertInterval_(*exterior, {}, {});
            assertInterval_(*exit, {}, {});

            const auto empty = traceOpticalReference(grid, {{0.5, 0.5}, {1.0, 0.0}, 1.0, 1.0});
            PPR_TEST_ASSERT(empty.has_value());
            assertInterval_(*empty, {}, {1.0, 1.0, 1.0});
        };

        PPR_UNIT_TEST (unavailable_invalid_and_nonfinite_inputs_report_errors) {
            std::array<OpticalCell, 1> cells{};
            OpticalGridView grid{cells, {0.0, 0.0}, 1.0, 1u, 1u, OpticalBoundary::vacuum};
            const OpticalRay ray{{0.0, 0.5}, {1.0, 0.0}, 0.0, 1.0};
            const auto missing = traceOpticalReference(grid, ray);
            PPR_TEST_ASSERT(not missing and missing.error() == std::errc::no_message);

            cells[0] = vacuum_();
            cells[0].m_volume_emission[0] = 1.0;
            const auto ambiguous = traceOpticalReference(grid, ray);
            PPR_TEST_ASSERT(not ambiguous and ambiguous.error() == std::errc::invalid_argument);

            cells[0] = vacuum_();
            grid.m_boundary = OpticalBoundary::unavailable;
            const auto boundary = traceOpticalReference(grid, ray);
            PPR_TEST_ASSERT(not boundary and boundary.error() == std::errc::invalid_argument);

            grid.m_boundary = OpticalBoundary::vacuum;
            OpticalRay invalid = ray;
            invalid.m_direction = {};
            const auto zero = traceOpticalReference(grid, invalid);
            PPR_TEST_ASSERT(not zero and zero.error() == std::errc::invalid_argument);

            invalid = ray;
            invalid.m_far_m = std::numeric_limits<double>::infinity();
            const auto infinite = traceOpticalReference(grid, invalid);
            PPR_TEST_ASSERT(not infinite and infinite.error() == std::errc::invalid_argument);

            RadianceInterval huge{{std::numeric_limits<double>::max(), 0.0, 0.0}, {1.0, 1.0, 1.0}};
            const auto overflow = composeIntervals(huge, huge);
            PPR_TEST_ASSERT(not overflow and overflow.error() == std::errc::result_out_of_range);
        };

        PPR_UNIT_TEST (collapsed_interior_grid_planes_are_rejected_before_tracing_or_capture) {
            std::array<OpticalCell, 4> cells{vacuum_(), vacuum_(), surface_({8.0, 4.0, 2.0}), vacuum_()};
            const OpticalGridView grid{cells, {9007199254740992.0, 0.0}, 1.5, 4u, 1u, OpticalBoundary::vacuum};
            const OpticalRay ray{{9007199254740992.0, 0.75}, {1.0, 0.0}, 0.0, 6.0};
            const auto reference = traceOpticalReference(grid, ray);
            const auto hierarchy = OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(not reference and reference.error() == std::errc::invalid_argument);
            PPR_TEST_ASSERT(not hierarchy and hierarchy.error() == std::errc::invalid_argument);
        };

        // ------------------------------------------------------------------
        // rendered macro geometry and immutable numeric capture
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (rounded_macro_capture_preserves_sources_saddles_and_world_metric) {
            std::array<u16, 4> labels{17u, 0u, 0u, 0u};
            std::array<OpticalTerrainMaterial, 5> materials{
                OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface_({8.0, 2.0, 1.0}), 17u, 1.0f},
                {surface_({1.0, 9.0, 2.0}), 23u, 1.0f}, {surface_({2.0, 1.0, 10.0}), 31u, 1.0f},
                {surface_({3.0, 4.0, 5.0}), 47u, 1.0f}};
            OpticalTerrainView input{labels, materials, {}, 1.0, 2u, 2u, 0u, OpticalBoundary::vacuum};
            const auto curved = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(curved.has_value());
            const OpticalRay ray{{1.375, 0.875}, {-1.0, 0.0}, 0.0, 1.0};
            const auto hit = traceOpticalTerrainReference(*curved, ray);
            PPR_TEST_ASSERT(hit.has_value() and hit->m_material == 17u and hit->m_hit_m.has_value());
            PPR_TEST_ASSERT(std::abs(*hit->m_hit_m - 0.5) < 1e-12);
            assertInterval_(hit->m_interval, {8.0, 2.0, 1.0}, {});

            materials[1].m_roundness = 0.0f;
            const auto straight = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(straight.has_value());
            const auto straight_hit = traceOpticalTerrainReference(*straight, ray);
            PPR_TEST_ASSERT(straight_hit.has_value() and straight_hit->m_hit_m.has_value());
            PPR_TEST_ASSERT(std::abs(*straight_hit->m_hit_m - 0.75) < 1e-12);
            materials[1].m_optics.m_surface_emission = {100.0, 100.0, 100.0};
            const auto retained = traceOpticalTerrainReference(*curved, ray);
            PPR_TEST_ASSERT(retained.has_value());
            assertInterval_(retained->m_interval, {8.0, 2.0, 1.0}, {});

            labels = {17u, 0u, 0u, 17u};
            const auto bridge = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(bridge.has_value());
            const auto bridge_hit = traceOpticalTerrainReference(*bridge, {{1.0, 1.0}, {1.0, 0.0}, 0.0, 0.125});
            PPR_TEST_ASSERT(bridge_hit.has_value() and bridge_hit->m_material == 17u);
            labels[3] = 23u;
            const auto islands = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(islands.has_value());
            const auto opening = traceOpticalTerrainReference(*islands, {{1.0, 1.0}, {1.0, 0.0}, 0.0, 0.125});
            PPR_TEST_ASSERT(opening.has_value() and not opening->m_hit_m);

            labels = {17u, 23u, 47u, 31u};
            const auto junction = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(junction.has_value());
            const auto selected = traceOpticalTerrainReference(*junction, {{0.75, 0.75}, {1.0, 0.0}, 0.0, 0.125});
            PPR_TEST_ASSERT(selected.has_value() and selected->m_material == 17u);

            input.m_min_m = {-10.0, 7.0};
            input.m_cell_size_m = 0.25;
            const auto metric = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(metric.has_value());
            const auto metric_hit = traceOpticalTerrainReference(
                *metric, {{-9.8125, 7.1875}, {-1.0, 0.0}, 0.0, 0.03125});
            PPR_TEST_ASSERT(metric_hit.has_value() and metric_hit->m_material == 17u and metric_hit->m_hit_m == 0.0);
        };

        PPR_UNIT_TEST (rounded_macro_all_cases_have_declared_samples_and_strict_missing_boundaries) {
            std::array<u16, 4> labels{};
            std::array<OpticalTerrainMaterial, 2> materials{
                OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface_({4.0, 2.0, 1.0}), 17u, 0.0f}};
            OpticalTerrainView input{labels, materials, {}, 1.0, 2u, 2u, 0u, OpticalBoundary::vacuum};
            constexpr std::array<u32, 4> storage{0u, 1u, 3u, 2u};
            constexpr std::array<std::array<double, 2>, 4> positions{{{0.5, 0.5}, {1.5, 0.5}, {1.5, 1.5}, {0.5, 1.5}}};
            for (const float roundness: {0.0f, 0.5f, 1.0f}) {
                materials[1].m_roundness = roundness;
                for (u32 mask = 0u; mask < 16u; ++mask) {
                    for (u32 corner = 0u; corner < 4u; ++corner) {
                        labels[storage[corner]] = (mask & (1u << corner)) != 0u ? 17u : 0u;
                    }

                    const auto snapshot = OpticalTerrainSnapshot::capture(input);
                    PPR_TEST_ASSERT(snapshot.has_value());
                    for (u32 corner = 0u; corner < 4u; ++corner) {
                        const auto traced = traceOpticalTerrainReference(
                            *snapshot, {positions[corner], {1.0, 0.0}, 0.0, 0.0625});
                        PPR_TEST_ASSERT(traced.has_value());
                        PPR_TEST_ASSERT(traced->m_hit_m.has_value() == ((mask & (1u << corner)) != 0u));
                    }
                }
            }

            labels.fill(0u);
            input.m_boundary = OpticalBoundary::opaque;
            input.m_outside_material = 17u;
            const auto black_exterior = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(black_exterior.has_value());
            const auto exterior = traceOpticalTerrainReference(*black_exterior, {{-1.0, 1.0}, {1.0, 0.0}, 0.0, 0.25});
            PPR_TEST_ASSERT(exterior.has_value());
            assertInterval_(exterior->m_interval, {}, {});

            labels[0] = 99u;
            const auto missing = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(not missing and missing.error() == std::errc::no_message);
            labels[0] = 17u;
            materials[1].m_optics.m_kind = OpticalKind::absorbing;
            const auto medium = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(not medium and medium.error() == std::errc::not_supported);
            materials[1].m_optics = {};
            const auto unavailable = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::not_supported);
        };

        PPR_UNIT_TEST (rounded_macro_one_cell_wall_door_and_global_seam_preserve_occlusion) {
            std::vector<u16> labels(129u * 3u);
            for (u32 y = 0u; y < 3u; ++y) {
                labels[std::size_t(y) * 129u + 127u] = 17u;
            }

            const std::array<OpticalTerrainMaterial, 2> materials{
                OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface_({4.0, 2.0, 1.0}), 17u, 0.5f}};
            const OpticalTerrainView input{labels, materials, {}, 1.0, 129u, 3u, 0u, OpticalBoundary::vacuum};
            const auto closed = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(closed.has_value());
            const OpticalRay across_seam{{126.25, 1.5}, {1.0, 0.0}, 0.0, 2.5};
            const auto blocked = traceOpticalTerrainReference(*closed, across_seam);
            PPR_TEST_ASSERT(blocked.has_value() and blocked->m_material == 17u);
            assertInterval_(blocked->m_interval, {4.0, 2.0, 1.0}, {});

            labels[129u + 127u] = 0u;
            const auto open = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(open.has_value());
            const auto doorway = traceOpticalTerrainReference(*open, across_seam);
            PPR_TEST_ASSERT(doorway.has_value() and not doorway->m_hit_m);
            assertInterval_(doorway->m_interval, {}, {1.0, 1.0, 1.0});

            const auto negative = traceOpticalTerrainReference(*closed, {{128.75, 1.5}, {-1.0, 0.0}, 0.0, 2.5});
            PPR_TEST_ASSERT(negative.has_value() and negative->m_material == 17u);
        };

        PPR_UNIT_TEST (rounded_macro_double_near_tangency_is_not_a_certified_miss) {
            const std::array<u16, 4> labels{17u, 0u, 0u, 0u};
            const std::array<OpticalTerrainMaterial, 2> materials{
                OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface_({4.0, 2.0, 1.0}), 17u, 1.0f}};
            const auto snapshot = OpticalTerrainSnapshot::capture(
                {labels, materials, {}, 1.0, 2u, 2u, 0u, OpticalBoundary::vacuum});
            PPR_TEST_ASSERT(snapshot.has_value());
            for (const double y: {std::nextafter(0.875, 0.0), 0.875, std::nextafter(0.875, 1.0)}) {
                const auto uncertain = traceOpticalTerrainReference(*snapshot, {{0.875, y}, {-1.0, 1.0}, 0.0, 0.125});
                PPR_TEST_ASSERT(not uncertain and uncertain.error() == std::errc::argument_out_of_domain);
            }
        };

        PPR_UNIT_TEST (terrain_parametric_endpoints_preserve_identity_metric_and_terminal_reach) {
            const std::array<u16, 4> labels{17u, 0u, 0u, 0u};
            const std::array<OpticalTerrainMaterial, 2> materials{
                OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface_({8.0, 2.0, 1.0}), 17u, 0.5f}};
            const OpticalTerrainView input{
                labels, materials, {1000000.0, -1000000.0}, 0.25, 4u, 1u, 0u, OpticalBoundary::vacuum};
            const auto snapshot = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(snapshot.has_value());

            // The raw vector has length1/2. Lambda is deliberately not metres
            // or normalized distance; both subpaths share the same endpoint.
            const std::array<double, 2> probe{4.5, 0.5}, direction{-0.5, 0.0};
            const double near = 2.0;
            const std::array<double, 2> endpoint{probe[0] + near * direction[0], probe[1] + near * direction[1]};
            const auto whole = traceOpticalTerrainParametricReference(*snapshot, {probe, direction, 0.0, 9.0});
            const auto suffix = traceOpticalTerrainParametricReference(*snapshot, {endpoint, direction, 0.0, 7.0});
            const auto stored = traceOpticalTerrainParametricReference(*snapshot, {probe, direction, near, 9.0});
            const auto prefix = traceOpticalTerrainConnectorReference(*snapshot, probe, endpoint);
            PPR_TEST_ASSERT(whole.has_value() and suffix.has_value() and stored.has_value() and prefix.has_value());
            PPR_TEST_ASSERT(whole->m_hit_m.has_value() and suffix->m_hit_m.has_value());
            assertInterval_(stored->m_interval, {8.0, 2.0, 1.0}, {});
            assertInterval_(suffix->m_interval, stored->m_interval.m_radiance, stored->m_interval.m_transmittance);
            PPR_TEST_ASSERT(std::abs(*whole->m_hit_m - *suffix->m_hit_m - near * 0.5 * input.m_cell_size_m) < 1e-12);

            const auto composed = composeIntervals(prefix->m_interval, stored->m_interval);
            PPR_TEST_ASSERT(composed.has_value());
            assertInterval_(*composed, whole->m_interval.m_radiance, whole->m_interval.m_transmittance);

            const auto identity = traceOpticalTerrainConnectorReference(*snapshot, endpoint, endpoint);
            const auto nonempty = traceOpticalTerrainConnectorReference(*snapshot, {0.625, 0.5}, {0.75, 0.5});
            PPR_TEST_ASSERT(identity.has_value() and nonempty.has_value());
            assertInterval_(identity->m_interval, {}, {1.0, 1.0, 1.0});
            assertInterval_(nonempty->m_interval, {8.0, 2.0, 1.0}, {});

            auto invalid_endpoint = endpoint;
            invalid_endpoint[0] = std::numeric_limits<double>::infinity();
            const auto invalid = traceOpticalTerrainConnectorReference(*snapshot, invalid_endpoint, invalid_endpoint);
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);

            RcEstimateRequest request{};
            request.m_width = 4u;
            request.m_height = 1u;
            request.m_cascade_count = 4u;
            request.m_c0_tile_width = 4u;
            request.m_c0_tile_height = 1u;
            request.m_live_generations = 1u;
            request.m_interval_bytes = 32u;
            request.m_optical_cell_bytes = 40u;
            request.m_budget_bytes = 1024u * 1024u;
            request.m_max_resource_bytes = 1024u * 1024u;
            const auto layout = estimateRcResources(request);
            PPR_TEST_ASSERT(layout.has_value() and not layout->m_admission_error);

            const auto &top = layout->m_cascades.back();
            const std::array<double, 2> padded_probe{
                0.5 + double((top.m_padded_probes[0] - 1u) * top.m_spacing_cells), 0.5};
            const double distance_bound = double(layout->m_terminal_far_cells);
            const auto too_short = traceOpticalTerrainParametricReference(
                *snapshot, {padded_probe, direction, double(top.m_near_cells), distance_bound});
            const auto complete = traceOpticalTerrainParametricReference(
                *snapshot, {padded_probe, direction, double(top.m_near_cells), 2.0 * distance_bound});
            PPR_TEST_ASSERT(too_short.has_value() and not too_short->m_hit_m);
            PPR_TEST_ASSERT(complete.has_value() and complete->m_hit_m.has_value());
            assertInterval_(complete->m_interval, {8.0, 2.0, 1.0}, {});

            const OpticalRay physical{
                {input.m_min_m[0] + padded_probe[0] * input.m_cell_size_m,
                    input.m_min_m[1] + padded_probe[1] * input.m_cell_size_m},
                direction, double(top.m_near_cells) * 0.5 * input.m_cell_size_m,
                2.0 * distance_bound * 0.5 * input.m_cell_size_m};
            const auto physical_trace = traceOpticalTerrainReference(*snapshot, physical);
            PPR_TEST_ASSERT(physical_trace.has_value() and physical_trace->m_hit_m.has_value());
            PPR_TEST_ASSERT(std::abs(*physical_trace->m_hit_m - *complete->m_hit_m) < 1e-12);
            assertInterval_(
                physical_trace->m_interval, complete->m_interval.m_radiance, complete->m_interval.m_transmittance);
        };

        PPR_UNIT_TEST (terrain_parametric_hit_distances_reject_overflow_and_positive_collapse) {
            const std::array<u16, 1> solid_labels{17u}, empty_labels{0u};
            const std::array<OpticalTerrainMaterial, 2> materials{
                OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface_({8.0, 2.0, 1.0}), 17u, 0.0f}};
            const auto reject_distance = [&](const OpticalTerrainSnapshot &snapshot,
                                             const OpticalTerrainParametricRay &ray) {
                const auto result = traceOpticalTerrainParametricReference(snapshot, ray);
                PPR_TEST_ASSERT(not result and result.error() == std::errc::result_out_of_range);
            };

            // A finite lambda and finite metres-per-lambda do not imply their
            // physical hit distance is finite. Check source hits and both BLACK
            // exterior events independently before publishing any hit_m value.
            OpticalTerrainView input{solid_labels, materials, {}, 1e308, 1u, 1u, 17u, OpticalBoundary::vacuum};
            const auto distant_surface = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(distant_surface.has_value());
            reject_distance(*distant_surface, {{-100.0, 0.5}, {1.0, 0.0}, 0.0, 102.0});

            input.m_boundary = OpticalBoundary::opaque;
            const auto distant_entry = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(distant_entry.has_value());
            reject_distance(*distant_entry, {{-100.0, 0.5}, {1.0, 0.0}, 2.0, 102.0});

            input.m_labels = empty_labels;
            input.m_outside_material = 0u;
            input.m_cell_size_m = 1.7e308;
            const auto distant_exit = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(distant_exit.has_value());
            reject_distance(*distant_exit, {{0.0, 0.0}, {0.5, 0.5}, 0.0, 3.0});

            // This metric still has two distinct positive half-cell planes.
            // A positive lambda1/8 times it collapses below the smallest double;
            // source, exterior-entry and exterior-exit hits must not report0.
            input.m_labels = solid_labels;
            input.m_outside_material = 17u;
            input.m_boundary = OpticalBoundary::vacuum;
            input.m_cell_size_m = 2.0 * std::numeric_limits<double>::denorm_min();
            const auto tiny_surface = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(tiny_surface.has_value());
            reject_distance(*tiny_surface, {{-0.125, 0.5}, {1.0, 0.0}, 0.0, 1.25});

            input.m_boundary = OpticalBoundary::opaque;
            const auto tiny_entry = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(tiny_entry.has_value());
            reject_distance(*tiny_entry, {{-100.0, 0.5}, {1.0, 0.0}, 0.125, 102.0});

            input.m_labels = empty_labels;
            input.m_outside_material = 0u;
            const auto tiny_exit = OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(tiny_exit.has_value());
            reject_distance(*tiny_exit, {{0.875, 0.5}, {1.0, 0.0}, 0.0, 1.0});
        };

        // ------------------------------------------------------------------
        // hierarchy and traversal parity
        // ------------------------------------------------------------------

        void assertParity_(const OpticalGridView &grid, const OpticalHierarchy &hierarchy, const OpticalRay &ray) {
            const auto reference = traceOpticalReference(grid, ray);
            const auto accelerated = traceOpticalAccelerated(hierarchy, ray);
            PPR_TEST_ASSERT(reference.has_value() and accelerated.has_value());
            assertInterval_(accelerated->m_interval, reference->m_radiance, reference->m_transmittance);
            PPR_TEST_ASSERT(accelerated->m_segments <= u64{grid.m_width} + grid.m_height);
        }

        PPR_UNIT_TEST (hierarchical_dda_matches_slabs_across_directions_ties_and_intervals) {
            std::array<OpticalCell, 35> cells{};
            for (std::size_t i = 0u; i < cells.size(); ++i) {
                if (i % 11u == 0u) {
                    cells[i] = surface_({2.0, 4.0, 8.0});
                } else if (i % 3u == 0u) {
                    cells[i] = OpticalCell{{0.3, 0.7, 1.1}, {0.2, 0.4, 0.6}, {}, OpticalKind::absorbing};
                } else {
                    cells[i] = vacuum_();
                }
            }

            OpticalGridView grid{cells, {-2.25, 5.5}, 1.0 / 3.0, 7u, 5u, OpticalBoundary::vacuum};
            const std::array<std::array<double, 2>, 12> directions{
                std::array<double, 2>{1.0, 0.0}, {-1.0, 0.0}, {0.0, 1.0}, {0.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0},
                {1.0, -1.0}, {-1.0, -1.0}, {0.123, 0.987}, {-0.731, 0.154}, {0.00001, -1.0}, {-1.0, 0.00001}};
            const std::array<std::array<double, 2>, 9> cell_origins{
                std::array<double, 2>{0.0, 0.0}, {7.0, 5.0}, {3.0, 2.0}, {0.5, 0.5}, {6.5, 4.5}, {-1.0, 2.0},
                {8.0, 2.0}, {3.0, -1.0}, {3.0, 6.0}};

            for (const OpticalBoundary boundary: {OpticalBoundary::vacuum, OpticalBoundary::opaque}) {
                grid.m_boundary = boundary;
                const auto hierarchy = OpticalHierarchy::capture(grid);
                PPR_TEST_ASSERT(hierarchy.has_value());

                for (const auto &position: cell_origins) {
                    const std::array<double, 2> origin{
                        grid.m_min_m[0] + position[0] * grid.m_cell_size_m,
                        grid.m_min_m[1] + position[1] * grid.m_cell_size_m};
                    for (const auto &direction: directions) {
                        assertParity_(grid, *hierarchy, {origin, direction, 0.0, 4.0});
                        assertParity_(grid, *hierarchy, {origin, direction, 0.25, 0.75});
                        assertParity_(grid, *hierarchy, {origin, direction, 1.0, 1.0});
                    }
                }
            }
        };

        PPR_UNIT_TEST (hierarchy_snapshot_requires_explicit_rebuild_after_optical_edits) {
            std::array<OpticalCell, 64> cells{};
            cells.fill(vacuum_());
            OpticalGridView grid{cells, {0.0, 0.0}, 1.0, 8u, 8u, OpticalBoundary::vacuum};
            const auto original = OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(original.has_value());
            const OpticalRay ray{{0.0, 4.5}, {1.0, 0.0}, 0.0, 8.0};
            const auto skipped = traceOpticalAccelerated(*original, ray);
            PPR_TEST_ASSERT(skipped.has_value());
            assertInterval_(skipped->m_interval, {}, {1.0, 1.0, 1.0});
            PPR_TEST_ASSERT(skipped->m_segments == 1u and skipped->m_vacuum_blocks == 1u);

            cells[4u * 8u + 4u] = surface_({7.0, 3.0, 1.0});
            const auto old_snapshot = traceOpticalAccelerated(*original, ray);
            PPR_TEST_ASSERT(old_snapshot.has_value());
            assertInterval_(old_snapshot->m_interval, {}, {1.0, 1.0, 1.0});

            const auto rebuilt = OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(rebuilt.has_value());
            const auto new_snapshot = traceOpticalAccelerated(*rebuilt, ray);
            PPR_TEST_ASSERT(new_snapshot.has_value());
            assertInterval_(new_snapshot->m_interval, {7.0, 3.0, 1.0}, {});
            assertParity_(grid, *rebuilt, ray);

            cells[4u * 8u + 4u] = vacuum_();
            const auto cleared = OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(cleared.has_value());
            assertParity_(grid, *cleared, ray);

            cells[0] = OpticalCell{};
            const auto unavailable = OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::no_message);
        };

        PPR_UNIT_TEST (long_transmissive_and_emitting_runs_have_no_128_step_cutoff) {
            const OpticalCell medium{{0.01, 0.02, 0.0}, {0.02, 0.03, 0.25}, {}, OpticalKind::absorbing};
            const std::vector<OpticalCell> cells(513u, medium);
            const OpticalGridView grid{cells, {-100.0, 7.0}, 1.0, 513u, 1u, OpticalBoundary::vacuum};
            const auto hierarchy = OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(hierarchy.has_value());
            const OpticalRay ray{{-100.0, 7.5}, {1.0, 0.0}, 0.0, 513.0};
            const auto accelerated = traceOpticalAccelerated(*hierarchy, ray);
            PPR_TEST_ASSERT(accelerated.has_value());
            const double red_transmission = std::exp(-5.13);
            const double green_transmission = std::exp(-10.26);
            assertInterval_(
                accelerated->m_interval, {2.0 * (1.0 - red_transmission), 1.5 * (1.0 - green_transmission), 128.25},
                {red_transmission, green_transmission, 1.0});
            PPR_TEST_ASSERT(accelerated->m_segments == 513u and accelerated->m_vacuum_blocks == 0u);
            assertParity_(grid, *hierarchy, ray);

            const OpticalRay reverse{{413.0, 7.5}, {-1.0, 0.0}, 0.0, 513.0};
            assertParity_(grid, *hierarchy, reverse);

            OpticalRay invalid = ray;
            invalid.m_direction = {};
            const auto zero = traceOpticalAccelerated(*hierarchy, invalid);
            PPR_TEST_ASSERT(not zero and zero.error() == std::errc::invalid_argument);
        };

        // ------------------------------------------------------------------
        // resource admission fixtures
        // ------------------------------------------------------------------

        RcEstimateRequest estimateRequest_() {
            RcEstimateRequest request{};
            request.m_interval_bytes = 24u;
            request.m_optical_cell_bytes = 16u;
            request.m_scalar_cell_bytes = 4u;
            request.m_additional_peak_bytes = 100u;
            request.m_additional_largest_resource_bytes = 100u;
            request.m_budget_bytes = 20000u;
            request.m_max_resource_bytes = 10000u;
            request.m_width = 3u;
            request.m_height = 2u;
            request.m_cascade_count = 3u;
            request.m_live_generations = 2u;
            request.m_retained_c1_generations = 1u;
            request.m_c0_tile_width = 2u;
            request.m_c0_tile_height = 1u;
            return request;
        }

        PPR_UNIT_TEST (padded_layout_streams_c0_and_counts_all_live_generations) {
            const auto estimate = estimateRcResources(estimateRequest_());
            PPR_TEST_ASSERT(estimate.has_value() and not estimate->m_admission_error);
            PPR_TEST_ASSERT(estimate->m_cascades.size() == 3u);
            PPR_TEST_ASSERT(estimate->m_peak_bytes == 6484u);
            PPR_TEST_ASSERT(estimate->m_all_levels_bytes_per_generation == 4992u);
            PPR_TEST_ASSERT(estimate->m_streaming_working_bytes == 4608u);
            PPR_TEST_ASSERT(estimate->m_retained_c1_bytes == 1536u);
            PPR_TEST_ASSERT(estimate->m_largest_resource_bytes == 3072u);

            const auto &base = estimate->m_cascades[0];
            PPR_TEST_ASSERT(base.m_logical_records == 48u and base.m_resident_records == 16u);
            PPR_TEST_ASSERT(base.m_near_cells == 0u and base.m_far_cells == 1u);
            const auto &middle = estimate->m_cascades[1];
            PPR_TEST_ASSERT(middle.m_padded_probes[0] == 2u and middle.m_padded_probes[1] == 2u);
            PPR_TEST_ASSERT(middle.m_high_guards[0] == 0u and middle.m_high_guards[1] == 1u);
            const auto &top = estimate->m_cascades[2];
            PPR_TEST_ASSERT(top.m_nominal_probes[0] == 1u and top.m_padded_probes[0] == 2u);
            PPR_TEST_ASSERT(top.m_directions == 32u and top.m_near_cells == 2u and top.m_far_cells == 4u);
        };

        PPR_UNIT_TEST (fixed_parity_capacity_counts_nonadjacent_level_maxima) {
            auto request = estimateRequest_();
            request.m_width = 8u;
            request.m_height = 8u;
            request.m_cascade_count = 5u;
            request.m_c0_tile_width = 1u;
            request.m_c0_tile_height = 1u;
            request.m_budget_bytes = 100000u;
            request.m_max_resource_bytes = 12288u;
            const auto estimate = estimateRcResources(request);
            PPR_TEST_ASSERT(estimate.has_value() and not estimate->m_admission_error);
            PPR_TEST_ASSERT(estimate->m_adjacent_live_bytes == 18432u);
            PPR_TEST_ASSERT(estimate->m_parity_buffer_bytes[0] == 12288u);
            PPR_TEST_ASSERT(estimate->m_parity_buffer_bytes[1] == 9600u);
            PPR_TEST_ASSERT(estimate->m_streaming_working_bytes == 21888u);
            PPR_TEST_ASSERT(estimate->m_peak_bytes == 34148u);
        };

        PPR_UNIT_TEST (terminal_guard_reach_extends_past_nominal_radius_to_world_exit) {
            auto request = estimateRequest_();
            const auto small = estimateRcResources(request);
            PPR_TEST_ASSERT(small.has_value());
            PPR_TEST_ASSERT(small->m_cascades.back().m_far_cells == 4u);
            PPR_TEST_ASSERT(small->m_terminal_far_cells == 9u);

            std::array<OpticalCell, 6> cells{};
            cells.fill(vacuum_());
            cells[0] = surface_({8.0, 4.0, 2.0});
            const OpticalGridView grid{cells, {0.0, 0.0}, 1.0, 3u, 2u, OpticalBoundary::vacuum};
            const OpticalRay nominal{{4.5, 4.5}, {-1.0, -1.0}, 2.0, 4.0};
            const auto truncated = traceOpticalReference(grid, nominal);
            PPR_TEST_ASSERT(truncated.has_value());
            assertInterval_(*truncated, {}, {1.0, 1.0, 1.0});

            OpticalRay complete = nominal;
            complete.m_far_m = double(small->m_terminal_far_cells);
            const auto extended = traceOpticalReference(grid, complete);
            const auto hierarchy = OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(extended.has_value() and hierarchy.has_value());
            assertInterval_(*extended, {8.0, 4.0, 2.0}, {});
            assertParity_(grid, *hierarchy, complete);

            request.m_width = 4096u;
            request.m_height = 4096u;
            request.m_cascade_count = 14u;
            request.m_c0_tile_width = 128u;
            request.m_c0_tile_height = 128u;
            const auto full = estimateRcResources(request);
            PPR_TEST_ASSERT(full.has_value());
            PPR_TEST_ASSERT(full->m_cascades.back().m_far_cells == 8192u);
            PPR_TEST_ASSERT(full->m_terminal_far_cells == 16385u);
            PPR_TEST_ASSERT(double(full->m_terminal_far_cells) >= std::hypot(8192.5, 8192.5));
        };

        PPR_UNIT_TEST (admission_retains_demand_and_rejects_overflow_or_short_reach) {
            auto request = estimateRequest_();
            request.m_budget_bytes = 6483u;
            const auto budget = estimateRcResources(request);
            PPR_TEST_ASSERT(budget.has_value() and budget->m_admission_error == std::errc::no_buffer_space);
            PPR_TEST_ASSERT(budget->m_peak_bytes == 6484u);

            request.m_budget_bytes = 6484u;
            request.m_max_resource_bytes = 3071u;
            const auto largest = estimateRcResources(request);
            PPR_TEST_ASSERT(largest.has_value() and largest->m_admission_error == std::errc::no_buffer_space);

            request.m_max_resource_bytes = 3072u;
            const auto exact = estimateRcResources(request);
            PPR_TEST_ASSERT(exact.has_value() and not exact->m_admission_error);

            request.m_interval_bytes = std::numeric_limits<u64>::max();
            const auto overflow = estimateRcResources(request);
            PPR_TEST_ASSERT(not overflow and overflow.error() == std::errc::value_too_large);

            request = estimateRequest_();
            request.m_cascade_count = 2u;
            const auto short_reach = estimateRcResources(request);
            PPR_TEST_ASSERT(not short_reach and short_reach.error() == std::errc::invalid_argument);

            request.m_cascade_count = 62u;
            const auto shift = estimateRcResources(request);
            PPR_TEST_ASSERT(not shift and shift.error() == std::errc::invalid_argument);
        };

    }
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // lighting contracts suite
    // ------------------------------------------------------------------

    const UnitTest lighting_contracts = UnitTest::Named("lighting") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::LightingTests::contour_roundness_mean_is_explicit_binary32_rte,
            detail::LightingTests::ordered_intervals_preserve_rgb_transmission_and_identity,
            detail::LightingTests::analytic_absorbing_slabs_use_order_and_metre_lengths,
            detail::LightingTests::opaque_emission_is_visible_before_stop_and_hides_far_sources,
            detail::LightingTests::corner_and_axis_ties_have_one_half_open_owner,
            detail::LightingTests::explicit_world_boundary_and_finite_intervals,
            detail::LightingTests::unavailable_invalid_and_nonfinite_inputs_report_errors,
            detail::LightingTests::collapsed_interior_grid_planes_are_rejected_before_tracing_or_capture,
            detail::LightingTests::rounded_macro_capture_preserves_sources_saddles_and_world_metric,
            detail::LightingTests::rounded_macro_all_cases_have_declared_samples_and_strict_missing_boundaries,
            detail::LightingTests::rounded_macro_one_cell_wall_door_and_global_seam_preserve_occlusion,
            detail::LightingTests::rounded_macro_double_near_tangency_is_not_a_certified_miss,
            detail::LightingTests::terrain_parametric_endpoints_preserve_identity_metric_and_terminal_reach,
            detail::LightingTests::terrain_parametric_hit_distances_reject_overflow_and_positive_collapse,
            detail::LightingTests::hierarchical_dda_matches_slabs_across_directions_ties_and_intervals,
            detail::LightingTests::hierarchy_snapshot_requires_explicit_rebuild_after_optical_edits,
            detail::LightingTests::long_transmissive_and_emitting_runs_have_no_128_step_cutoff,
            detail::LightingTests::padded_layout_streams_c0_and_counts_all_live_generations,
            detail::LightingTests::fixed_parity_capacity_counts_nonadjacent_level_maxima,
            detail::LightingTests::terminal_guard_reach_extends_past_nominal_radius_to_world_exit,
            detail::LightingTests::admission_retains_demand_and_rejects_overflow_or_short_reach,
        });
    };

    const UnitTest &lightingTests() noexcept {
        return lighting_contracts;
    }
}
