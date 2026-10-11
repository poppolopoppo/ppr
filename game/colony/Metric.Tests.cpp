module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.sim;
import game.colony.metric;
import std;

namespace pP::game::tests::detail::metric {
    using namespace colony;

    PPR_UNIT_TEST (centres_and_half_open_boundaries) {
        for (u32 i = 0u; i < sim::kWorldEdge; ++i) {
            const auto centre = fineCellCentre({i, i});
            PPR_TEST_ASSERT(centre.has_value());
            PPR_TEST_ASSERT(centre->m_x == (static_cast<double>(i) + 0.5) / 3.0);
            const auto round_trip = metreToFineCell(*centre);
            PPR_TEST_ASSERT(round_trip.has_value());
            PPR_TEST_ASSERT((*round_trip == sim::GlobalCellPos{i, i}));

            const auto boundary = fineCellBoundaryMetres(i);
            PPR_TEST_ASSERT(boundary.has_value());
            const auto on = metreToFineCell({*boundary, *boundary});
            const auto after = metreToFineCell({std::nextafter(*boundary, kWorldExtentMetres), *boundary});
            PPR_TEST_ASSERT(on.has_value() and after.has_value());
            PPR_TEST_ASSERT(on->m_x == i and after->m_x == i);
            if (i != 0u) {
                const auto before = metreToFineCell({std::nextafter(*boundary, 0.0), *boundary});
                PPR_TEST_ASSERT(before.has_value() and before->m_x == i - 1u);
            }
        }
        const auto last = metreToFineCell({std::nextafter(kWorldExtentMetres, 0.0), 0.0});
        PPR_TEST_ASSERT(last.has_value() and last->m_x == sim::kWorldEdge - 1u);
        PPR_TEST_ASSERT(not metreToFineCell({kWorldExtentMetres, 0.0}).has_value());
        PPR_TEST_ASSERT(not metreToFineCell({-0.001, 0.0}).has_value());
        PPR_TEST_ASSERT(not metreToFineCell({0.0, std::numeric_limits<double>::infinity()}).has_value());
        PPR_TEST_ASSERT(not metreToFineCell({std::numeric_limits<double>::quiet_NaN(), 0.0}).has_value());
        PPR_TEST_ASSERT(not fineCellCentre({sim::kWorldEdge, 0u}).has_value());
        PPR_TEST_ASSERT(*fineCellBoundaryMetres(sim::kWorldEdge) == kWorldExtentMetres);
        PPR_TEST_ASSERT(not fineCellBoundaryMetres(sim::kWorldEdge + 1u).has_value());
    };

    PPR_UNIT_TEST (construction_is_global_and_crosses_chunks) {
        const auto footprint = constructionFootprint({42u, 42u});
        PPR_TEST_ASSERT(footprint.has_value());
        PPR_TEST_ASSERT((footprint->m_begin == sim::GlobalCellPos{126u, 126u}));
        PPR_TEST_ASSERT((footprint->m_end == sim::GlobalCellPos{129u, 129u}));
        PPR_TEST_ASSERT(sim::chunkOf(footprint->m_begin).m_x == 0u);
        PPR_TEST_ASSERT(sim::chunkOf({footprint->m_end.m_x - 1u, footprint->m_end.m_y - 1u}).m_x == 1u);
        for (u32 y = 126u; y < 129u; ++y) {
            for (u32 x = 126u; x < 129u; ++x) {
                const auto tile = constructionTileOf({x, y});
                PPR_TEST_ASSERT(tile.has_value() and tile->m_x == 42u and tile->m_y == 42u);
            }
        }
        const auto last = constructionFootprint({1364u, 1364u});
        PPR_TEST_ASSERT(last.has_value() and last->m_end.m_x == 4095u and last->m_end.m_y == 4095u);
        PPR_TEST_ASSERT(not constructionTileOf({4095u, 0u}).has_value());
        PPR_TEST_ASSERT(not constructionTileOf({0u, 4095u}).has_value());
        PPR_TEST_ASSERT(not constructionFootprint({1365u, 0u}).has_value());
        PPR_TEST_ASSERT(not constructionFootprint({1364u, 0u}, {2u, 1u}).has_value());
        PPR_TEST_ASSERT(not constructionFootprint({0u, 0u}, {0u, 1u}).has_value());
        PPR_TEST_ASSERT(not constructionFootprint({(std::numeric_limits<u32>::max)(), 0u}).has_value());
        PPR_TEST_ASSERT(not constructionFootprint({0u, 0u}, {(std::numeric_limits<u32>::max)(), 1u}).has_value());
        const auto multi = constructionFootprint({1u, 2u}, {2u, 3u});
        PPR_TEST_ASSERT(multi.has_value() and multi->m_begin.m_x == 3u and multi->m_end.m_y == 15u);
    };

    PPR_UNIT_TEST (construction_drag_snaps_and_clamps_from_the_anchor_tile) {
        const auto single = constructionDragFootprint({127u, 128u}, {128u, 126u}, 32u);
        PPR_TEST_ASSERT(single.has_value());
        PPR_TEST_ASSERT((single->m_begin == sim::GlobalCellPos{126u, 126u}));
        PPR_TEST_ASSERT((single->m_end == sim::GlobalCellPos{129u, 129u}));
        const auto forward = constructionDragFootprint({127u, 127u}, {500u, 500u}, 32u);
        PPR_TEST_ASSERT(forward.has_value());
        PPR_TEST_ASSERT((forward->m_begin == sim::GlobalCellPos{126u, 126u}));
        PPR_TEST_ASSERT((forward->m_end == sim::GlobalCellPos{156u, 156u}));
        const auto reverse = constructionDragFootprint({127u, 127u}, {0u, 0u}, 32u);
        PPR_TEST_ASSERT(reverse.has_value());
        PPR_TEST_ASSERT((reverse->m_begin == sim::GlobalCellPos{99u, 99u}));
        PPR_TEST_ASSERT((reverse->m_end == sim::GlobalCellPos{129u, 129u}));
        PPR_TEST_ASSERT(not constructionDragFootprint({4095u, 0u}, {0u, 0u}, 32u));
        PPR_TEST_ASSERT(not constructionDragFootprint({0u, 0u}, {4095u, 0u}, 32u));
        PPR_TEST_ASSERT(not constructionDragFootprint({0u, 0u}, {0u, 0u}, 2u));
        const auto corner = constructionDragFootprint({4094u, 4094u}, {4094u, 4094u}, 32u);
        PPR_TEST_ASSERT(corner.has_value() and corner->m_end.m_x == 4095u and corner->m_end.m_y == 4095u);
    };

    PPR_UNIT_TEST (discrete_dimensions_volume_and_metric_identity) {
        for (u32 i = 1u; i <= sim::kWorldEdge; ++i) {
            const double metres = static_cast<double>(i) / kFineCellsPerMetre;
            const auto length = discreteFineCellLength(metres);
            PPR_TEST_ASSERT(length.has_value() and *length == i);
            if (i == 1u or i == 128u or i == sim::kWorldEdge) {
                PPR_TEST_ASSERT(not discreteFineCellLength(std::nextafter(metres, 0.0)).has_value());
                PPR_TEST_ASSERT(
                    not discreteFineCellLength(std::nextafter(metres, std::numeric_limits<double>::infinity()))
                        .has_value());
            }
        }
        PPR_TEST_ASSERT(*discreteFineCellLength(1.0) == 3u);
        PPR_TEST_ASSERT(*discreteFineCellLength(3.0) == 9u);
        PPR_TEST_ASSERT(*discreteFineCellLength(1.0 / 3.0) == 1u);
        PPR_TEST_ASSERT(not discreteFineCellLength(std::nextafter(1.0 / 3.0, 1.0)).has_value());
        PPR_TEST_ASSERT(not discreteFineCellLength(std::nextafter(1.0 / 3.0, 0.0)).has_value());
        PPR_TEST_ASSERT(not discreteFineCellLength(0.1).has_value());
        PPR_TEST_ASSERT(not discreteFineCellLength(-1.0).has_value());
        PPR_TEST_ASSERT(not discreteFineCellLength(std::numeric_limits<double>::infinity()).has_value());
        PPR_TEST_ASSERT(std::abs(9.0 * kNominalFineCellVolumeM3 - 1.0) < 1e-15);
        PPR_TEST_ASSERT(not validateWorldMetric({}));
        PPR_TEST_ASSERT(validateWorldMetric({1u, 1u, 1.0}) == std::make_error_code(std::errc::not_supported));
        PPR_TEST_ASSERT(validateWorldMetric({3u, 3u, 2.0}) == std::make_error_code(std::errc::not_supported));
    };
}

namespace pP::game::tests {
    const UnitTest metric = UnitTest::Named("metric") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::metric::centres_and_half_open_boundaries,
                detail::metric::construction_is_global_and_crosses_chunks,
                detail::metric::construction_drag_snaps_and_clamps_from_the_anchor_tile,
                detail::metric::discrete_dimensions_volume_and_metric_identity});
    };
    const UnitTest &gameMetricTests() noexcept {
        return metric;
    }
}
