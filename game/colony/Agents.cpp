module;

#include "pP/Macros.h"
#include "Colony.Elements.h"

module game.colony.agents;

import engine.core;
import engine.sim;
import game.colony.buildings;
import game.colony.pathfinding;

import std;

namespace pP::colony {
    namespace {
        // Self-contained splitmix64 (worldgen rationale: core RNG is
        // hardware-seeded and RngStream is snapshot-local, so spawns carry
        // their own stream derived from the colony seed).
        [[nodiscard]] constexpr u64 mixOnce(u64 value) noexcept {
            value ^= value >> 30u;
            value *= 0xBF58476D1CE4E5B9u;
            value ^= value >> 27u;
            value *= 0x94D049BB133111EBu;
            value ^= value >> 31u;
            return value;
        }

        [[nodiscard]] u64 nextStream(u64 &state) noexcept {
            state += 0x9E3779B97F4A7C15u;
            return mixOnce(state);
        }

        [[nodiscard]] bool cellsOpen(const sim::ChunkGrid &grid, const u32 x, const u32 y) noexcept {
            return isWalkable(grid, {x, y}) and isWalkable(grid, {x, y + 1u});
        }

        [[nodiscard]] bool onLadder(const sim::Registry &registry, const float x, const float y) {
            for (const auto &[entity, errand]: registry.view<BuildErrand>()) {
                (void) entity;
                if (not errand.m_print.m_ladder) {
                    continue;
                }
                const float min_x = static_cast<float>(errand.m_print.m_min.m_x);
                const float min_y = static_cast<float>(errand.m_print.m_min.m_y);
                const float max_x = min_x + static_cast<float>(errand.m_print.m_width);
                const float max_y = min_y + static_cast<float>(errand.m_print.m_height);
                if (x >= min_x and x < max_x and y >= min_y and y < max_y) {
                    return true;
                }
            }
            return false;
        }

        struct Desired {
            EGoal m_goal{EGoal::seek};
            sim::GlobalCellPos m_site{};
            u32 m_op{kOpMove};
        };

        [[nodiscard]] Desired desiredGoal(const sim::ChunkGrid &grid, const Agent &agent, const Needs &needs,
                                          const sim::BodyState &pose, sim::Registry &registry) {
            Desired want{};
            const sim::GlobalCellPos here{static_cast<u32>(pose.m_x), static_cast<u32>(pose.m_y)};
            const auto here_cell = grid.getCell(here);
            if (here_cell and here_cell->m_element == kElementCaustic)
            {
                want.m_goal = EGoal::flee;
                want.m_op = kOpMove;
                for (u32 radius = 10u; radius < 200u; radius += 5u) {
                    for (u32 dy = 0u; dy <= radius; ++dy) {
                        const u32 dx = radius - dy;
                        const u32 x = here.m_x >= dx ? here.m_x - dx : here.m_x + dx;
                        const u32 y = here.m_y >= dy ? here.m_y - dy : here.m_y + dy;
                        if (isWalkable(grid, {x, y})) {
                            want.m_site = {x, y};
                            return want;
                        }
                    }
                }
                want.m_site = agent.m_spawn;
                return want;
            }
            if (needs.m_rest < kRestThreshold) {
                want.m_goal = EGoal::seek;
                want.m_site = agent.m_spawn;
                want.m_op = kOpRest;
                return want;
            }
            if (needs.m_o2 < kO2Threshold) {
                want.m_goal = EGoal::seek;
                want.m_site = agent.m_spawn;
                want.m_op = kOpMove;
                u32 best_y = sim::kWorldEdge;
                for (u32 y = 0u; y < agent.m_spawn.m_y; y += 4u) {
                    for (u32 x = 0u; x < sim::kWorldEdge; x += 16u) {
                        if (isWalkable(grid, {x, y}) and y < best_y) {
                            best_y = y;
                            want.m_site = {x, y};
                        }
                    }
                }
                return want;
            }
            want.m_goal = EGoal::work;
            want.m_op = kOpWork;
            bool errand = false;
            u32 best_distance = 0xFFFFFFFFu;
            for (const auto &[entity, row]: registry.view<BuildErrand>()) {
                (void) entity;
                if (row.m_progress >= row.m_total) {
                    continue;
                }
                // Nearest live errand by Manhattan distance: every agent gets
                // the shortest completable trip instead of all piling onto
                // the first errand regardless of distance.
                const sim::GlobalCellPos center{
                    row.m_print.m_min.m_x + row.m_print.m_width / 2u,
                    row.m_print.m_min.m_y + row.m_print.m_height / 2u};
                const u32 dx = center.m_x >= here.m_x ? center.m_x - here.m_x : here.m_x - center.m_x;
                const u32 dy = center.m_y >= here.m_y ? center.m_y - here.m_y : here.m_y - center.m_y;
                if (dx + dy >= best_distance) {
                    continue;
                }
                // Agents stand next to walls to build them: the footprint
                // center itself turns solid as the errand applies, so a goal
                // there would complete blocked and the agent would never move.
                // Stage on the nearest walkable cell instead (deterministic
                // diamond scan, center fallback preserves old behavior).
                best_distance = dx + dy;
                want.m_site = center;
                for (u32 radius = 1u; radius <= 40u; ++radius) {
                    bool staged = false;
                    for (u32 step_y = 0u; step_y <= radius and not staged; ++step_y) {
                        const u32 step_x = radius - step_y;
                        const sim::GlobalCellPos candidates[4] = {
                            {center.m_x + step_x, center.m_y + step_y},
                            {center.m_x - step_x, center.m_y + step_y},
                            {center.m_x + step_x, center.m_y - step_y},
                            {center.m_x - step_x, center.m_y - step_y}};
                        for (const sim::GlobalCellPos candidate: candidates) {
                            if (isWalkable(grid, candidate)) {
                                want.m_site = candidate;
                                staged = true;
                                break;
                            }
                        }
                    }
                    if (staged) {
                        break;
                    }
                }
                errand = true;
            }
            if (not errand) {
                want.m_goal = EGoal::seek;
                want.m_op = kOpMove;
                want.m_site = agent.m_spawn;
            }
            return want;
        }
    }

    std::error_code registerAgentComponents(sim::Registry &registry) {
        const auto agent = registry.registerComponent<Agent>();
        if (not agent) {
            return agent.error();
        }
        const auto needs = registry.registerComponent<Needs>();
        if (not needs) {
            return needs.error();
        }
        const auto plan = registry.registerComponent<Plan>();
        if (not plan) {
            return plan.error();
        }
        return {};
    }

    std::error_code spawnAgents(
        const sim::ChunkGrid &grid, sim::Registry &registry, const u64 seed, const u32 count) {
        u64 stream = seed + 0x9E3779B97F4A7C15u;
        for (u32 agent = 0u; agent < count; ++agent) {
            sim::GlobalCellPos spawn{sim::kWorldEdge, sim::kWorldEdge};
            for (u32 attempt = 0u; attempt < 256u and spawn.m_x >= sim::kWorldEdge; ++attempt) {
                const u32 x = static_cast<u32>(nextStream(stream) % (sim::kWorldEdge - 1u));
                const u32 y = static_cast<u32>(nextStream(stream) % (sim::kWorldEdge - 2u));
                for (u32 dy = 0u; dy < 64u and spawn.m_x >= sim::kWorldEdge; ++dy) {
                    for (u32 dx = 0u; dx < 64u; ++dx) {
                        const u32 cx = (x + dx) % sim::kWorldEdge;
                        const u32 cy = (y + dy) % (sim::kWorldEdge - 1u);
                        if (cellsOpen(grid, cx, cy)) {
                            spawn = {cx, cy};
                            break;
                        }
                    }
                }
            }
            if (spawn.m_x >= sim::kWorldEdge) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
            const sim::Entity entity = registry.create();
            const u64 key = (static_cast<u64>(entity.m_generation) << 32u) | entity.m_index;
            registry.emplace<Agent>(entity, Agent{.m_key = key, .m_spawn = spawn});
            registry.emplace<Needs>(entity, Needs{});
            registry.emplace<sim::BodyState>(entity,
                sim::BodyState{
                    static_cast<float>(spawn.m_x) + 0.5f, static_cast<float>(spawn.m_y) + 0.5f, 0.0f, 0.0f, 0.0f, 0.0f
                });
            registry.emplace<sim::BodyDefinition>(entity,
                sim::BodyDefinition{
                    .m_motion = sim::EMotionKind::dynamic,
                    .m_geometry = sim::EBodyGeometry::box,
                    .m_half_width = 0.4f,
                    .m_half_height = 0.9f,
                    .m_enable_sleep = false,
                    .m_fixed_rotation = true
                });
        }
        return {};
    }

    void planAgent(const sim::ChunkGrid &grid, sim::Registry &registry) {
        for (const auto &[entity, agent]: registry.view<Agent>()) {
            const Needs *needs = registry.get<Needs>(entity);
            const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
            if (needs == nullptr or pose == nullptr) {
                continue;
            }

            const Desired want = desiredGoal(grid, agent, *needs, *pose, registry);
            Plan *plan = registry.get<Plan>(entity);
            if (plan != nullptr and plan->m_goal == want.m_goal and plan->m_site == want.m_site) {
                continue;
            }

            const sim::GlobalCellPos from{static_cast<u32>(pose->m_x), static_cast<u32>(pose->m_y)};
            const u32 replans = plan != nullptr ? plan->m_replans + 1u : 0u;
            Plan next{};
            next.m_goal = want.m_goal;
            next.m_site = want.m_site;
            next.m_actions[0] = AgentAction{.m_op = want.m_op, .m_target = want.m_site};
            next.m_action_count = 1u;
            next.m_replans = replans;
            if (plan != nullptr) {
                *plan = next;
            } else {
                registry.emplace<Plan>(entity, next);
            }

            PathReq *req = registry.get<PathReq>(entity);
            const PathReq fresh{.m_entity = entity, .m_from = from, .m_to = want.m_site, .m_caps = kPathCapsWalk};
            if (req != nullptr) {
                *req = fresh;
            } else {
                registry.emplace<PathReq>(entity, fresh);
            }
            (void) registry.remove<PathComp>(entity);
            return;
        }
    }

    std::error_code agentIntent(sim::Registry &registry, const sim::Entity entity, Intent &intent) {
        intent = Intent{};
        const Agent *agent = registry.get<Agent>(entity);
        Plan *plan = registry.get<Plan>(entity);
        const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
        Needs *needs = registry.get<Needs>(entity);
        if (agent == nullptr or plan == nullptr or pose == nullptr or needs == nullptr) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        if (plan->m_pc >= plan->m_action_count) {
            return {};
        }
        const AgentAction &action = plan->m_actions[plan->m_pc];
        const float dx = static_cast<float>(action.m_target.m_x) + 0.5f - pose->m_x;
        const float dy = static_cast<float>(action.m_target.m_y) + 0.5f - pose->m_y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < 1.5f) {
            if (action.m_op == kOpRest) {
                needs->m_o2 = 100.0f;
                needs->m_rest = 100.0f;
            }
            ++plan->m_pc;
            return {};
        }
        PathComp *path = registry.get<PathComp>(entity);
        float step_x = dx;
        float step_y = dy;
        if (path != nullptr and not path->m_partial and path->m_cursor < path->m_count) {
            sim::GlobalCellPos cell = path->m_pts[path->m_cursor];
            step_x = static_cast<float>(cell.m_x) + 0.5f - pose->m_x;
            step_y = static_cast<float>(cell.m_y) + 0.5f - pose->m_y;
            if (step_x * step_x + step_y * step_y < kWaypointRadius * kWaypointRadius) {
                ++path->m_cursor;
                if (path->m_cursor >= path->m_count) {
                    ++plan->m_pc;
                    return {};
                }
                cell = path->m_pts[path->m_cursor];
                step_x = static_cast<float>(cell.m_x) + 0.5f - pose->m_x;
                step_y = static_cast<float>(cell.m_y) + 0.5f - pose->m_y;
            }
        } else if (path != nullptr and path->m_partial and path->m_count == 0u) {
            return {};
        }
        const float length = std::sqrt(step_x * step_x + step_y * step_y);
        if (length <= 0.0f) {
            return {};
        }
        intent.m_velocity_x = step_x / length * agent->m_speed;
        intent.m_velocity_y = step_y / length * agent->m_speed;
        if (onLadder(registry, pose->m_x, pose->m_y)) {
            intent.m_climbing = true;
            intent.m_velocity_y = agent->m_speed;
            if (step_x * step_x < 0.04f) {
                intent.m_velocity_x = 0.0f;
            }
        } else {
            intent.m_velocity_y = 0.0f;
        }
        needs->m_o2 -= 0.05f;
        needs->m_rest -= 0.1f;
        if (needs->m_o2 < 0.0f) {
            needs->m_o2 = 0.0f;
        }
        if (needs->m_rest < 0.0f) {
            needs->m_rest = 0.0f;
        }
        return {};
    }
}
