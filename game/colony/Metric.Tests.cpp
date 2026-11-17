module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.sim;
import game.colony.metric;
import std;

namespace pP::game::tests::detail::metric {
    // ------------------------------------------------------------------
    // metric
    // ------------------------------------------------------------------

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

    PPR_UNIT_TEST (selected_world_bounds_preserve_physical_cell_scale) {
        for (const u32 edge: {1024u, 2048u, 4096u}) {
            const double extent = static_cast<double>(edge) / kFineCellsPerMetre;
            const auto centre = fineCellCentre({edge - 1u, edge - 1u}, edge);
            PPR_TEST_ASSERT(centre.has_value());
            PPR_TEST_ASSERT(centre->m_x == (static_cast<double>(edge) - 0.5) / kFineCellsPerMetre);

            const auto last = metreToFineCell({std::nextafter(extent, 0.0), 0.0}, kFineCellsPerMetre, edge);
            PPR_TEST_ASSERT(last.has_value() and last->m_x == edge - 1u);
            PPR_TEST_ASSERT(not metreToFineCell({extent, 0.0}, kFineCellsPerMetre, edge));
            PPR_TEST_ASSERT(not fineCellCentre({edge, 0u}, edge));
            PPR_TEST_ASSERT(*fineCellBoundaryMetres(edge, edge) == extent);
            PPR_TEST_ASSERT(not fineCellBoundaryMetres(edge + 1u, edge));
        }

        PPR_TEST_ASSERT(not metreToFineCell({0.0, 0.0}, kFineCellsPerMetre, 1536u));
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
                detail::metric::selected_world_bounds_preserve_physical_cell_scale,
                detail::metric::discrete_dimensions_volume_and_metric_identity});
    };

    const UnitTest &gameMetricTests() noexcept {
        return metric;
    }
}
