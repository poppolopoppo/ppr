module;

#include "pP/Macros.h"
#include "Colony.Elements.h"

module game.colony.buildings;

import engine.core;
import engine.sim;
import game.colony.agents;
import game.colony.pathfinding;

import std;

namespace pP::colony {
    namespace {
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

    std::error_code validateFootprint(const Footprint &print) noexcept {
        if (print.m_width == 0u or
            print.m_height == 0u or
            print.m_width > kMaxFootprintEdge or
            print.m_height > kMaxFootprintEdge) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (print.m_element > kElementVent) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (print.m_ladder and print.m_solid) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 far_x = static_cast<u64>(print.m_min.m_x) + print.m_width;
        const u64 far_y = static_cast<u64>(print.m_min.m_y) + print.m_height;
        if (far_x > sim::kWorldEdge or far_y > sim::kWorldEdge) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return {};
    }

    void advanceErrands(sim::ChunkGrid &grid, sim::Registry &registry, Pathfinder &finder,
                        Array<sim::ChunkPos> &touched, const u32 cell_budget) {
        Array<sim::Entity> live{};
        for (const auto &[entity, errand]: registry.view<BuildErrand>()) {
            if (errand.m_progress < errand.m_total) {
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
