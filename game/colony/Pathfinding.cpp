module;

#include "Colony.Elements.h"

module game.colony.pathfinding;

import engine.core;
import engine.sim;

import std;

namespace pP::colony {
    namespace {
        [[nodiscard]] u32 packCell(const u32 x, const u32 y) noexcept {
            return y * sim::kWorldEdge + x;
        }

        [[nodiscard]] u32 packPos(const sim::GlobalCellPos pos) noexcept {
            return packCell(pos.m_x, pos.m_y);
        }

        [[nodiscard]] sim::GlobalCellPos unpackCell(const u32 packed) noexcept {
            return {packed % sim::kWorldEdge, packed / sim::kWorldEdge};
        }

        [[nodiscard]] u32 manhattan(const u32 packed, const sim::GlobalCellPos goal) noexcept {
            const sim::GlobalCellPos cell = unpackCell(packed);
            const u32 dx = cell.m_x >= goal.m_x ? cell.m_x - goal.m_x : goal.m_x - cell.m_x;
            const u32 dy = cell.m_y >= goal.m_y ? cell.m_y - goal.m_y : goal.m_y - cell.m_y;
            return dx + dy;
        }

        [[nodiscard]] bool elementAt(const sim::ChunkGrid &grid, const u32 x, const u32 y, u16 &element) noexcept {
            const sim::ChunkPos chunk{x / sim::kChunkEdge, y / sim::kChunkEdge};
            const std::optional<std::span<const sim::Cell> > cells = grid.residentCells(chunk);
            if (not cells) {
                element = kElementVacuum;
                return true;
            }
            if (cells->size() != sim::kCellsPerChunk) {
                return false;
            }
            const u32 local_x = x % sim::kChunkEdge;
            const u32 local_y = y % sim::kChunkEdge;
            element = (*cells)[local_y * sim::kChunkEdge + local_x].m_element;
            return true;
        }

        [[nodiscard]] bool markedCell(const Array<u64> &bits, const u32 packed) noexcept {
            return (bits[packed / 64u] & (u64{1u} << (packed % 64u))) != 0u;
        }

        void markCell(Array<u64> &bits, const u32 packed) noexcept {
            bits[packed / 64u] |= u64{1u} << (packed % 64u);
        }

        void emitResult(sim::Registry &registry, const sim::Entity entity, PathComp comp) noexcept {
            if (not registry.isAlive(entity)) {
                return;
            }
            if (registry.get<PathComp>(entity) != nullptr) {
                return;
            }
            registry.emplace<PathComp>(entity, comp);
        }

        void finishSearch(sim::Registry &registry, Pathfinder &finder, const u32 end_node, const bool partial) noexcept {
            const sim::Entity entity = finder.m_entity;
            Array<u32> chain{};
            u32 node = end_node;
            while (node != none_v and node < finder.m_nodes.size()) {
                chain.push_back(finder.m_nodes[node].m_packed);
                node = finder.m_nodes[node].m_parent;
            }
            PathComp comp{};
            comp.m_partial = partial;
            if (chain.size() > 1u) {
                u32 total = 0u;
                if (chain.size() - 1u <= static_cast<std::size_t>(kMaxPathPts)) {
                    total = static_cast<u32>(chain.size() - 1u);
                } else {
                    total = kMaxPathPts;
                    comp.m_partial = true;
                }
                for (u32 point = 0u; point < total; ++point) {
                    comp.m_pts[point] = unpackCell(chain[chain.size() - 2u - point]);
                }
                comp.m_count = total;
            }
            finder = Pathfinder{};
            emitResult(registry, entity, comp);
        }
    }

    bool isWalkable(const sim::ChunkGrid &grid, const sim::GlobalCellPos pos) noexcept {
        if (not sim::isInsideWorld(pos)) {
            return false;
        }
        u16 element = kElementVacuum;
        if (not elementAt(grid, pos.m_x, pos.m_y, element)) {
            return false;
        }
        return element == kElementVacuum;
    }

    std::error_code registerPathfindingComponents(sim::Registry &registry) {
        const auto req = registry.registerComponent<PathReq>();
        if (not req) {
            return req.error();
        }
        const auto comp = registry.registerComponent<PathComp>();
        if (not comp) {
            return comp.error();
        }
        return {};
    }

    void stepPathfinding(const sim::ChunkGrid &grid, sim::Registry &registry, Pathfinder &finder, const u32 budget) {
        if (finder.m_has_search) {
            const PathReq *active_req = registry.get<PathReq>(finder.m_entity);
            const bool matches = active_req != nullptr and
                registry.isAlive(finder.m_entity) and
                active_req->m_from == finder.m_from and
                active_req->m_to == finder.m_to and
                active_req->m_caps == finder.m_caps and
                registry.get<PathComp>(finder.m_entity) == nullptr;
            if (not matches) {
                finder = Pathfinder{};
            }
        }

        if (not finder.m_has_search) {
            sim::Entity pending{};
            bool found = false;
            for (const auto &[entity, req]: registry.view<PathReq>()) {
                (void) req;
                if (registry.get<PathComp>(entity) == nullptr) {
                    pending = entity;
                    found = true;
                    break;
                }
            }
            if (not found) {
                return;
            }
            const PathReq *req = registry.get<PathReq>(pending);
            if (req == nullptr or not registry.isAlive(pending)) {
                return;
            }
            const bool usable = sim::isInsideWorld(req->m_from) and
                sim::isInsideWorld(req->m_to) and
                (req->m_caps & kPathCapsWalk) != 0u and
                (req->m_caps & ~kPathCapsWalk) == 0u;
            const bool endpoints_open = usable and isWalkable(grid, req->m_from) and isWalkable(grid, req->m_to);
            if (not endpoints_open) {
                emitResult(registry, pending, PathComp{.m_partial = true});
                return;
            }
            if (req->m_from == req->m_to) {
                emitResult(registry, pending, PathComp{});
                return;
            }
            finder.m_has_search = true;
            finder.m_entity = pending;
            finder.m_from = req->m_from;
            finder.m_to = req->m_to;
            finder.m_caps = req->m_caps;
            finder.m_nodes.push_back(SearchNode{.m_packed = packPos(req->m_from), .m_parent = none_v});
            finder.m_frontier.push_back(0u);
            finder.m_bits.resize(kSearchBitWords);
            std::fill(finder.m_bits.data(), finder.m_bits.data() + finder.m_bits.size(), u64{});
            markCell(finder.m_bits, packPos(req->m_from));
            finder.m_closest = 0u;
            finder.m_closest_dist = manhattan(packPos(req->m_from), req->m_to);
        }

        u32 used = 0u;
        while (used < budget and finder.m_head < finder.m_frontier.size()) {
            const u32 current_node = finder.m_frontier[finder.m_head];
            ++finder.m_head;
            ++used;
            if (current_node >= finder.m_nodes.size()) {
                finishSearch(registry, finder, finder.m_closest, true);
                return;
            }
            const u32 current = finder.m_nodes[current_node].m_packed;
            if (current == packPos(finder.m_to)) {
                finishSearch(registry, finder, current_node, false);
                return;
            }
            const sim::GlobalCellPos cell = unpackCell(current);
            // Fixed x-major neighbor order: deterministic across runs.
            constexpr i32 kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &[dx, dy]: kSteps) {
                const i32 next_x = static_cast<i32>(cell.m_x) + dx;
                const i32 next_y = static_cast<i32>(cell.m_y) + dy;
                if (next_x < 0 or
                    next_y < 0 or
                    static_cast<u32>(next_x) >= sim::kWorldEdge or
                    static_cast<u32>(next_y) >= sim::kWorldEdge) {
                    continue;
                }
                const u32 x = static_cast<u32>(next_x);
                const u32 y = static_cast<u32>(next_y);
                u16 element = kElementVacuum;
                if (not elementAt(grid, x, y, element) or element != kElementVacuum)
                {
                    continue;
                }
                const u32 next = packCell(x, y);
                if (markedCell(finder.m_bits, next)) {
                    continue;
                }
                if (finder.m_nodes.size() >= kMaxSearchNodes) {
                    finishSearch(registry, finder, finder.m_closest, true);
                    return;
                }
                const u32 distance = manhattan(next, finder.m_to);
                if (distance < finder.m_closest_dist) {
                    finder.m_closest_dist = distance;
                    finder.m_closest = static_cast<u32>(finder.m_nodes.size());
                }
                markCell(finder.m_bits, next);
                finder.m_frontier.push_back(static_cast<u32>(finder.m_nodes.size()));
                finder.m_nodes.push_back(SearchNode{.m_packed = next, .m_parent = current_node});
            }
        }

        if (finder.m_head < finder.m_frontier.size()) {
            return;
        }
        finishSearch(registry, finder, finder.m_closest, true);
    }

    PathCounts pathCounts(sim::Registry &registry) {
        PathCounts counts{};
        for (const auto &[entity, comp]: registry.view<PathComp>()) {
            (void) entity;
            if (not comp.m_partial) {
                ++counts.m_paths;
            } else if (comp.m_count > 0u) {
                ++counts.m_partials;
            } else {
                ++counts.m_blocked;
            }
        }
        return counts;
    }
}
