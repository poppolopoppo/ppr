module;
#include "pP/Macros.h"

module game.colony.metric;

import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyMetric, info, none)

    std::error_code validateWorldMetric(const WorldMetric &metric) {
        if (metric != WorldMetric{}) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::not_supported));
        }
        return {};
    }

    Expected<MetrePoint> fineCellCentre(const sim::GlobalCellPos position) {
        if (not sim::isInsideWorld(position)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }
        return MetrePoint{
            (static_cast<double>(position.m_x) + 0.5) / kFineCellsPerMetre,
            (static_cast<double>(position.m_y) + 0.5) / kFineCellsPerMetre};
    }

    Expected<sim::GlobalCellPos> metreToFineCell(const MetrePoint position, const u32 cells_per_metre) {
        if ((cells_per_metre != 1u and cells_per_metre != kFineCellsPerMetre) or
            not std::isfinite(position.m_x) or
            not std::isfinite(position.m_y)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::invalid_argument));
        }
        if (position.m_x < 0.0 or
            position.m_y < 0.0 or
            position.m_x >= static_cast<double>(sim::kWorldEdge) / cells_per_metre or
            position.m_y >= static_cast<double>(sim::kWorldEdge) / cells_per_metre) {
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
        if (x >= sim::kWorldEdge or y >= sim::kWorldEdge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }
        return sim::GlobalCellPos{x, y};
    }

    Expected<double> fineCellBoundaryMetres(const u32 boundary) {
        if (boundary > sim::kWorldEdge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }
        return static_cast<double>(boundary) / kFineCellsPerMetre;
    }

    Expected<u32> discreteFineCellLength(const double metres) {
        if (not std::isfinite(metres) or metres < 0.0 or metres > kWorldExtentMetres) {
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

    Expected<FineCellRect> constructionFootprint(const ConstructionTile tile, const TileExtent extent) {
        constexpr u32 complete_tiles = sim::kWorldEdge / kFineCellsPerConstructionTile;
        if (extent.m_width == 0u or
            extent.m_height == 0u or
            tile.m_x >= complete_tiles or
            tile.m_y >= complete_tiles or
            extent.m_width > complete_tiles - tile.m_x or
            extent.m_height > complete_tiles - tile.m_y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }
        return FineCellRect{
            {tile.m_x * kFineCellsPerConstructionTile, tile.m_y * kFineCellsPerConstructionTile},
            {(tile.m_x + extent.m_width) * kFineCellsPerConstructionTile,
                (tile.m_y + extent.m_height) * kFineCellsPerConstructionTile}};
    }

    Expected<FineCellRect> constructionDragFootprint(
        const sim::GlobalCellPos anchor, const sim::GlobalCellPos cursor, const u32 max_fine_edge) {
        if (max_fine_edge < kFineCellsPerConstructionTile) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::errc::invalid_argument);
        }
        const auto begin = constructionTileOf(anchor);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, begin);
        const auto end = constructionTileOf(cursor);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, end);
        constexpr u32 complete_tiles = sim::kWorldEdge / kFineCellsPerConstructionTile;
        const i32 limit = static_cast<i32>(std::min(max_fine_edge / kFineCellsPerConstructionTile, complete_tiles));
        const auto clamp_axis = [limit](const u32 origin, const u32 target) {
            const i32 from = static_cast<i32>(origin);
            return static_cast<u32>(std::clamp(static_cast<i32>(target), from - limit + 1, from + limit - 1));
        };
        const u32 x = clamp_axis(begin->m_x, end->m_x);
        const u32 y = clamp_axis(begin->m_y, end->m_y);
        const u32 left = std::min(begin->m_x, x);
        const u32 bottom = std::min(begin->m_y, y);
        return constructionFootprint(
            {left, bottom}, {std::max(begin->m_x, x) - left + 1u, std::max(begin->m_y, y) - bottom + 1u});
    }

    Expected<ConstructionTile> constructionTileOf(const sim::GlobalCellPos position) {
        if (not sim::isInsideWorld(position)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, std::make_error_code(std::errc::result_out_of_range));
        }
        const ConstructionTile tile{
            position.m_x / kFineCellsPerConstructionTile, position.m_y / kFineCellsPerConstructionTile};
        const auto footprint = constructionFootprint(tile);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMetric, footprint);
        return tile;
    }
}
