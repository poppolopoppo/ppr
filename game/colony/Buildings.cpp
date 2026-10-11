module;

#include "pP/Macros.h"
#include "Colony.Elements.h"

module game.colony.buildings;

import engine.core;
import engine.sim;
import game.colony.agents;
import game.colony.pathfinding;
import game.colony.content;
import game.colony.metric;

import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyBuilding, info, none)
    namespace {
        [[nodiscard]] Expected<bool> actorOverlaps(sim::Registry &registry, const Footprint &print) {
            const double left = static_cast<double>(print.m_min.m_x) / 3.0;
            const double bottom = static_cast<double>(print.m_min.m_y) / 3.0;
            const double right = static_cast<double>(print.m_min.m_x + print.m_width) / 3.0;
            const double top = static_cast<double>(print.m_min.m_y + print.m_height) / 3.0;
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                (void) agent;
                const auto *const pose = registry.get<sim::BodyState>(entity);
                const auto *const body = registry.get<sim::BodyDefinition>(entity);
                if (pose == nullptr or
                    body == nullptr or
                    not std::isfinite(pose->m_x) or
                    not std::isfinite(pose->m_y) or
                    not std::isfinite(body->m_half_width) or
                    not std::isfinite(body->m_half_height) or
                    body->m_half_width <= 0.0f or
                    body->m_half_height <= 0.0f) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
                }
                if (pose->m_x + body->m_half_width > left and
                    pose->m_x - body->m_half_width < right and
                    pose->m_y + body->m_half_height > bottom and
                    pose->m_y - body->m_half_height < top) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool chunkListed(const Array<sim::ChunkPos> &touched, const sim::ChunkPos pos) noexcept {
            for (const sim::ChunkPos listed: touched) {
                if (listed == pos) {
                    return true;
                }
            }
            return false;
        }
    }

    std::error_code registerBuildingComponents(sim::Registry &registry) {
        const auto print = registry.registerComponent<Footprint>();
        if (not print) {
            return print.error();
        }
        const auto errand = registry.registerComponent<BuildErrand>();
        if (not errand) {
            return errand.error();
        }
        return {};
    }

    std::error_code registerColonyComponents(sim::Registry &registry) {
        if (const std::error_code error = registerPathfindingComponents(registry)) {
            return error;
        }
        if (const std::error_code error = registerBuildingComponents(registry)) {
            return error;
        }
        if (const std::error_code error = registerAgentComponents(registry)) {
            return error;
        }
        const auto pose = registry.registerComponent<sim::BodyState>();
        if (not pose) {
            return pose.error();
        }
        const auto definition = registry.registerComponent<sim::BodyDefinition>();
        if (not definition) {
            return definition.error();
        }
        return {};
    }

    std::error_code validateFootprint(const Footprint &print, const ContentCatalog *const catalog) noexcept {
        if (print.m_width == 0u or
            print.m_height == 0u or
            print.m_width > kMaxFootprintEdge or
            print.m_height > kMaxFootprintEdge) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
        }
        if (catalog == nullptr) {
            if (print.m_element > kElementVent) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
            }
        } else if (print.m_solid or print.m_element != kElementVacuum) {
            const auto *const material = catalog->findMaterial(print.m_element);
            if (material == nullptr or
                material->m_representation != EMaterialRepresentation::terrain or
                material->m_phase != EMaterialPhase::solid) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
            }
        }
        if (print.m_ladder and print.m_solid) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
        }
        const u64 far_x = static_cast<u64>(print.m_min.m_x) + print.m_width;
        const u64 far_y = static_cast<u64>(print.m_min.m_y) + print.m_height;
        if (far_x > sim::kWorldEdge or far_y > sim::kWorldEdge) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
        }
        return {};
    }

    std::error_code validateConstruction(
        const sim::ChunkGrid &grid, sim::Registry &registry, const Footprint &print, const ContentCatalog &catalog) {
        PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, validateFootprint(print, &catalog));
        if (print.m_min.m_x % 3u != 0u or
            print.m_min.m_y % 3u != 0u or
            print.m_width % 3u != 0u or
            print.m_height % 3u != 0u) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
        }
        const auto bounds = constructionFootprint(
            {print.m_min.m_x / 3u, print.m_min.m_y / 3u}, {print.m_width / 3u, print.m_height / 3u});
        PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, bounds);
        for (u32 y = bounds->m_begin.m_y; y < bounds->m_end.m_y; ++y) {
            for (u32 x = bounds->m_begin.m_x; x < bounds->m_end.m_x; ++x) {
                const auto cell = grid.getCell({x, y});
                PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, cell);
                if (cell->m_element != kElementVacuum) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::device_or_resource_busy);
                }
            }
        }
        const auto overlap = actorOverlaps(registry, print);
        PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, overlap);
        if (*overlap) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::device_or_resource_busy);
        }
        for (const auto &[entity, errand]: registry.view<BuildErrand>()) {
            (void) entity;
            const auto &other = errand.m_print;
            if (errand.m_progress < errand.m_total and
                print.m_min.m_x < other.m_min.m_x + other.m_width and
                other.m_min.m_x < print.m_min.m_x + print.m_width and
                print.m_min.m_y < other.m_min.m_y + other.m_height and
                other.m_min.m_y < print.m_min.m_y + print.m_height) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::device_or_resource_busy);
            }
        }
        return {};
    }

    std::error_code advanceErrands(
        sim::ChunkGrid &grid, sim::Registry &registry, Pathfinder &finder, Array<sim::ChunkPos> &touched,
        const u32 cell_budget, const ContentCatalog *const catalog,
        const std::optional<std23::function_ref<std::error_code(sim::ChunkPos)>> before_write) {
        Array<sim::Entity> live{};
        for (const auto &[entity, errand]: registry.view<BuildErrand>()) {
            if (errand.m_total != errand.m_print.m_width * errand.m_print.m_height or
                errand.m_progress > errand.m_total) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
            }
            if (errand.m_progress < errand.m_total) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, validateFootprint(errand.m_print, catalog));
                if (catalog != nullptr) {
                    // Validate every remaining ID before this tick performs any writes.
                    for (u32 step = errand.m_progress; step < errand.m_total; ++step) {
                        const auto &print = errand.m_print;
                        const auto cell = grid.getCell(
                            {print.m_min.m_x + step % print.m_width, print.m_min.m_y + step / print.m_width});
                        PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, cell);
                        if (cell->m_element != kElementVacuum) {
                            const auto *const material = catalog->findMaterial(cell->m_element);
                            if (material == nullptr or
                                material->m_representation != EMaterialRepresentation::terrain or
                                material->m_phase != EMaterialPhase::solid) {
                                PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
                            }
                        }
                    }
                    if (errand.m_print.m_solid) {
                        const auto overlap = actorOverlaps(registry, errand.m_print);
                        PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, overlap);
                        if (*overlap) {
                            // Retain the reservation while an actor occupies it.
                            continue;
                        }
                    }
                }
                live.push_back(entity);
            }
        }
        u32 left = cell_budget;
        bool wrote = false;
        for (const sim::Entity entity: live) {
            BuildErrand *errand = registry.get<BuildErrand>(entity);
            if (errand == nullptr) {
                continue;
            }
            while (left > 0u and errand->m_progress < errand->m_total) {
                const u32 step = errand->m_progress;
                const u32 x = errand->m_print.m_min.m_x + step % errand->m_print.m_width;
                const u32 y = errand->m_print.m_min.m_y + step / errand->m_print.m_width;
                const auto current = grid.getCell({x, y});
                PPR_ASSERT(current.has_value());
                const u16 element = errand->m_print.m_solid ? errand->m_print.m_element : kElementVacuum;
                if (current->m_element == element) {
                    // Already the target: count progress without a write so
                    // markDirty's changed-content contract is never violated.
                    // Skips cost no budget and invalidate nothing.
                    ++errand->m_progress;
                    continue;
                }
                if (catalog != nullptr and current->m_element != kElementVacuum) {
                    const auto *const material = catalog->findMaterial(current->m_element);
                    if (material == nullptr or
                        material->m_representation != EMaterialRepresentation::terrain or
                        material->m_phase != EMaterialPhase::solid) {
                        PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, std::errc::invalid_argument);
                    }
                    // Recheck live cells: a pending build cannot overwrite terrain,
                    // and demolition cannot remove protected material.
                    if (errand->m_print.m_solid or not material->m_diggable.value_or(false)) {
                        ++errand->m_progress;
                        continue;
                    }
                }
                if (before_write) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyBuilding, (*before_write)(sim::chunkOf({x, y})));
                }
                const std::error_code set_error = grid.setCell({x, y}, sim::Cell{element, current->m_temperature});
                PPR_ASSERT(not set_error);
                const sim::ChunkPos chunk{x / sim::kChunkEdge, y / sim::kChunkEdge};
                grid.markDirty(chunk);
                if (not chunkListed(touched, chunk)) {
                    touched.push_back(chunk);
                }
                ++errand->m_progress;
                --left;
                wrote = true;
            }
            if (left == 0u) {
                break;
            }
        }
        if (wrote) {
            finder = Pathfinder{};
        }
        return {};
    }

    u32 liveErrandCount(sim::Registry &registry) {
        u32 live = 0u;
        for (const auto &[entity, errand]: registry.view<BuildErrand>()) {
            (void) entity;
            if (errand.m_progress < errand.m_total) {
                ++live;
            }
        }
        return live;
    }
}
