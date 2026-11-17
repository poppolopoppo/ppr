module;
#include "pP/Macros.h"

module game.colony.metric;

import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyMetric, info, none)

    // ------------------------------------------------------------------
    // validation
    // ------------------------------------------------------------------

    std::error_code validateWorldMetric(const WorldMetric &metric) {
        if (metric != WorldMetric{}) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::not_supported));
        }
        return {};
    }

    // ------------------------------------------------------------------
    // fine cells
    // ------------------------------------------------------------------

    Expected<MetrePoint> fineCellCentre(const sim::GlobalCellPos position, const u32 world_edge) {
        if (not sim::isSupportedWorldEdge(world_edge) or position.m_x >= world_edge or position.m_y >= world_edge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }

        return MetrePoint{
            (static_cast<double>(position.m_x) + 0.5) / kFineCellsPerMetre,
            (static_cast<double>(position.m_y) + 0.5) / kFineCellsPerMetre};
    }

    Expected<sim::GlobalCellPos> metreToFineCell(
        const MetrePoint position, const u32 cells_per_metre, const u32 world_edge) {
        if (not sim::isSupportedWorldEdge(world_edge) or
            (cells_per_metre != 1u and cells_per_metre != kFineCellsPerMetre) or
            not std::isfinite(position.m_x) or
            not std::isfinite(position.m_y)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::invalid_argument));
        }

        if (position.m_x < 0.0 or
            position.m_y < 0.0 or
            position.m_x >= static_cast<double>(world_edge) / cells_per_metre or
            position.m_y >= static_cast<double>(world_edge) / cells_per_metre) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }

        // The range guard precedes conversion, including the rounded final bound.
        const auto cell_axis = [cells_per_metre](const double metres) {
            auto index = static_cast<u32>(std::floor(metres * cells_per_metre));
            // Multiplication can round a neighbouring representable value onto
            // an integer. Compare with the same canonical boundaries we export.
            if (index > 0u and metres < static_cast<double>(index) / cells_per_metre) {
                --index;
            } else if (metres >= static_cast<double>(index + 1u) / cells_per_metre) {
                ++index;
            }
            return index;
        };

        const auto x = cell_axis(position.m_x);
        const auto y = cell_axis(position.m_y);
        if (x >= world_edge or y >= world_edge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }

        return sim::GlobalCellPos{x, y};
    }

    Expected<double> fineCellBoundaryMetres(const u32 boundary, const u32 world_edge) {
        if (not sim::isSupportedWorldEdge(world_edge) or boundary > world_edge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }

        return static_cast<double>(boundary) / kFineCellsPerMetre;
    }

    Expected<u32> discreteFineCellLength(const double metres, const u32 world_edge) {
        if (not sim::isSupportedWorldEdge(world_edge) or
            not std::isfinite(metres) or
            metres < 0.0 or
            metres > static_cast<double>(world_edge) / kFineCellsPerMetre) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::invalid_argument));
        }

        // Identify the nearest candidate, then require the exact exported
        // boundary. This does not round author input onto the lattice and also
        // avoids rejecting a canonical i/3 whose product rounds below i.
        const auto count = static_cast<u32>(std::round(metres * kFineCellsPerMetre));
        if (metres != static_cast<double>(count) / kFineCellsPerMetre) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::invalid_argument));
        }
        return count;
    }
}
