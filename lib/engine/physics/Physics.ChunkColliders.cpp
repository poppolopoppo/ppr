module;
#include "pP/Macros.h"

module engine.physics;

import :chunk_colliders;
import std;

namespace pP::physics {
    namespace {
        struct Vertex {
            i32 x{};
            i32 y{};

            [[nodiscard]] auto operator<=>(const Vertex &) const noexcept = default;
        };

        struct Edge {
            Vertex from;
            Vertex to;
        };

        [[nodiscard]] const ColliderChunkView *findChunk(const std::span<const ColliderChunkView> chunks,
                                                         const ColliderChunkPos pos) noexcept {
            const auto it = std::ranges::find(chunks, pos, &ColliderChunkView::m_pos);
            return it == chunks.end() ? nullptr : &*it;
        }

        [[nodiscard]] bool solid(const std::span<const ColliderChunkView> chunks, const i32 edge,
                                 const i32 x, const i32 y) noexcept {
            const auto floorDiv = [edge](const i32 v) { return v >= 0 ? v / edge : (v + 1) / edge - 1; };
            const ColliderChunkPos pos{floorDiv(x), floorDiv(y)};
            const ColliderChunkView *const chunk = findChunk(chunks, pos);
            if (not chunk) {
                return false;
            }
            const i32 local_x = x - pos.m_x * edge;
            const i32 local_y = y - pos.m_y * edge;
            return chunk->m_elements[static_cast<std::size_t>(local_y) * edge + local_x] != 0u;
        }

        template<class F>
        void cellEdges(const std::span<const ColliderChunkView> chunks, const i32 edge, const i32 x,
                       const i32 y, F &&append) {
            if (not solid(chunks, edge, x, y)) {
                return;
            }
            if (not solid(chunks, edge, x, y + 1)) {
                append(Edge{{x + 1, y + 1}, {x, y + 1}});
            }
            if (not solid(chunks, edge, x - 1, y)) {
                append(Edge{{x, y + 1}, {x, y}});
            }
            if (not solid(chunks, edge, x, y - 1)) {
                append(Edge{{x, y}, {x + 1, y}});
            }
            if (not solid(chunks, edge, x + 1, y)) {
                append(Edge{{x + 1, y}, {x + 1, y + 1}});
            }
        }

        [[nodiscard]] int turnRank(const Edge &incoming, const Edge &outgoing) noexcept {
            const int ax = incoming.to.x - incoming.from.x;
            const int ay = incoming.to.y - incoming.from.y;
            const int bx = outgoing.to.x - outgoing.from.x;
            const int by = outgoing.to.y - outgoing.from.y;
            const int cross = ax * by - ay * bx;
            return cross > 0 ? 0 : (cross == 0 ? 1 : 2);
        }

        [[nodiscard]] std::optional<Edge> exteriorEdge(const std::span<const ColliderChunkView> chunks,
                                                       const i32 edge, const ColliderChunkPos owner, const Vertex vertex, const Edge adjacent,
                                                       const bool before) {
            std::optional<Edge> best;
            int rank = 4;
            for (const i32 y: {vertex.y - 1, vertex.y}) {
                for (const i32 x: {vertex.x - 1, vertex.x}) {
                    const auto floorDiv = [edge](const i32 v) { return v >= 0 ? v / edge : (v + 1) / edge - 1; };
                    if (ColliderChunkPos{floorDiv(x), floorDiv(y)} == owner) {
                        continue;
                    }
                    cellEdges(chunks, edge, x, y, [&](const Edge candidate) {
                        if ((before ? candidate.to == vertex : candidate.from == vertex)) {
                            const int next_rank = before ? turnRank(candidate, adjacent) : turnRank(adjacent, candidate);
                            if (next_rank < rank) {
                                best = candidate;
                                rank = next_rank;
                            }
                        }
                    });
                }
            }
            return best;
        }

        [[nodiscard]] ChainPoint point(const Vertex p) noexcept {
            return {static_cast<float>(p.x), static_cast<float>(p.y)};
        }

        struct Polyline {
            std::vector<ChainPoint> points;
            ChainPoint before{};
            ChainPoint after{};
            bool loop{};
        };

        [[nodiscard]] std::vector<Polyline> extract(const std::span<const ColliderChunkView> chunks,
                                                    const ColliderChunkPos pos, const i32 edge) {
            std::vector<Edge> edges;
            const i32 origin_x = pos.m_x * edge;
            const i32 origin_y = pos.m_y * edge;
            for (i32 y = origin_y; y < origin_y + edge; ++y) {
                for (i32 x = origin_x; x < origin_x + edge; ++x) {
                    cellEdges(chunks, edge, x, y, [&](const Edge e) { edges.push_back(e); });
                }
            }
            std::vector<bool> used(edges.size());
            std::vector<Polyline> lines;
            const auto next = [&](const Vertex vertex, const Edge prior) -> std::size_t {
                std::size_t best = edges.size();
                int rank = 4;
                for (std::size_t i = 0; i < edges.size(); ++i) {
                    if (not used[i] and edges[i].from == vertex)
                    {
                        const int candidate = turnRank(prior, edges[i]);
                        if (candidate < rank) {
                            best = i;
                            rank = candidate;
                        }
                    }
                }
                return best;
            };
            for (int phase = 0; phase < 2; ++phase) {
                for (std::size_t seed = 0; seed < edges.size(); ++seed) {
                    if (used[seed]) {
                        continue;
                    }
                    const bool has_predecessor = std::ranges::any_of(edges,
                        [&](const Edge e) { return e.to == edges[seed].from; });
                    if (phase == 0 and has_predecessor) {
                        continue;
                    }
                    Polyline line;
                    const Vertex start = edges[seed].from;
                    line.points.push_back(point(start));
                    std::size_t cursor = seed;
                    std::size_t last_index = seed;
                    while (cursor != edges.size()) {
                        used[cursor] = true;
                        last_index = cursor;
                        const Edge current = edges[cursor];
                        if (current.to == start) {
                            line.loop = true;
                            break;
                        }
                        line.points.push_back(point(current.to));
                        cursor = next(current.to, current);
                    }
                    if (not line.loop) {
                        const Vertex end{
                            static_cast<i32>(line.points.back().m_x),
                            static_cast<i32>(line.points.back().m_y)
                        };
                        const Edge first = edges[seed];
                        const Edge last = edges[last_index];
                        const auto before = exteriorEdge(chunks, edge, pos, start, first, true);
                        const auto after = exteriorEdge(chunks, edge, pos, end, last, false);
                        line.before = before
                                          ? point(before->from)
                                          : point({
                                              2 * start.x - first.to.x,
                                              2 * start.y - first.to.y
                                          });
                        line.after = after
                                         ? point(after->to)
                                         : point({
                                             2 * end.x - last.from.x,
                                             2 * end.y - last.from.y
                                         });
                    }
                    lines.push_back(std::move(line));
                }
            }
            return lines;
        }
    }

    std::error_code ChunkColliders::rebuildDirtyChunks(Scene &scene, const std::span<const ColliderChunkView> chunks,
                                                       const std::span<const ColliderChunkPos> dirty, const u32 edge, const u32 max_chunks, u32 &processed) noexcept {
        processed = 0u;
        if (edge == 0u or edge > 1024u or max_chunks == 0u) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const auto &chunk = chunks[i];
            if (chunk.m_pos.m_x < 0 or
                chunk.m_pos.m_y < 0 or
                chunk.m_pos.m_x > 100000 / static_cast<i32>(edge) or
                chunk.m_pos.m_y > 100000 / static_cast<i32>(edge) or
                chunk.m_elements.size() != static_cast<std::size_t>(edge) * edge or
                std::ranges::find(chunks.begin(), chunks.begin() + i, chunk.m_pos, &ColliderChunkView::m_pos) !=
                    chunks.begin() + i) {
                return std::make_error_code(std::errc::invalid_argument);
            }
        }
        try {
            for (const ColliderChunkPos pos: dirty) {
                if (not findChunk(chunks, pos)) {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                for (const ColliderChunkPos adjacent: {
                         pos, ColliderChunkPos{pos.m_x - 1, pos.m_y},
                         ColliderChunkPos{pos.m_x + 1, pos.m_y}, ColliderChunkPos{pos.m_x, pos.m_y - 1},
                         ColliderChunkPos{pos.m_x, pos.m_y + 1}
                     }) {
                    if (findChunk(chunks, adjacent)) {
                        m_pending.insert(adjacent);
                    }
                }
            }
            while (not m_pending.empty() and processed < max_chunks) {
                const ColliderChunkPos pos = *m_pending.begin();
                const ColliderChunkView *const chunk = findChunk(chunks, pos);
                if (not chunk) {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                std::vector<ChainHandle> fresh;
                if (chunk->m_needed) {
                    const auto lines = extract(chunks, pos, static_cast<i32>(edge));
                    fresh.reserve(lines.size());
                    for (const Polyline &line: lines) {
                        const ChainDefinition definition{line.points, line.before, line.after, 0u, 1u, line.loop};
                        auto created = scene.createChunkChain(definition);
                        if (not created) {
                            for (const ChainHandle handle: fresh) {
                                (void) scene.destroyChunkChain(handle);
                            }
                            return created.error();
                        }
                        fresh.push_back(*created);
                    }
                }
                auto existing = m_chains.find(pos);
                if (existing == m_chains.end() and not fresh.empty()) {
                    try {
                        existing = m_chains.emplace(pos, std::vector<ChainHandle>{}).first;
                    } catch (...) {
                        for (const ChainHandle handle: fresh) {
                            (void) scene.destroyChunkChain(handle);
                        }
                        throw;
                    }
                }
                if (existing != m_chains.end()) {
                    for (const ChainHandle handle: existing->second) {
                        const std::error_code destroyed = scene.destroyChunkChain(handle);
                        if (destroyed) {
                            for (const ChainHandle created: fresh) {
                                (void) scene.destroyChunkChain(created);
                            }
                            return destroyed;
                        }
                    }
                    existing->second.swap(fresh);
                    if (existing->second.empty()) {
                        m_chains.erase(existing);
                    }
                }
                m_pending.erase(m_pending.begin());
                ++processed;
            }
        } catch (const std::bad_alloc &) {
            return std::make_error_code(std::errc::not_enough_memory);
        }
        return {};
    }

    std::error_code ChunkColliders::materializeForQuery(Scene &scene,
                                                        const std::span<const ColliderChunkView> chunks, const ColliderChunkPos pos, const u32 edge) noexcept {
        if (not findChunk(chunks, pos)) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        try {
            std::vector<ColliderChunkView> active(chunks.begin(), chunks.end());
            for (ColliderChunkView &chunk: active) {
                if (chunk.m_pos == pos) {
                    chunk.m_needed = true;
                }
            }
            std::set<ColliderChunkPos> delayed;
            delayed.swap(m_pending);
            try {
                m_pending.insert(pos);
                u32 processed{};
                const std::error_code result = rebuildDirtyChunks(scene, active, {}, edge, 1u, processed);
                m_pending.merge(delayed);
                return result;
            } catch (...) {
                m_pending.merge(delayed);
                throw;
            }
        } catch (const std::bad_alloc &) {
            return std::make_error_code(std::errc::not_enough_memory);
        }
    }

    std::size_t ChunkColliders::chainCount(const ColliderChunkPos pos) const noexcept {
        const auto it = m_chains.find(pos);
        return it == m_chains.end() ? 0u : it->second.size();
    }

    std::size_t ChunkColliders::pendingCount() const noexcept {
        return m_pending.size();
    }
}
