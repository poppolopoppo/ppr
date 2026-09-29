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

        // Fixed-capacity open-addressing index: chunk position -> ordinal in
        // the caller views span. Built once per rebuildDirtyChunks call and
        // threaded through findChunk/solid/cellEdges/exteriorEdge/validation,
        // so the per-cell probes in extract() (up to 16,384 cells x 5 solid()
        // calls) and the duplicate validation scan drop from O(views) each to
        // O(1) average probes.
        //
        // Bound proof: Sim.ChunkGrid fixes the live world at kChunkCount =
        // 32 x 32 = 1024 chunks (Sim.ChunkGrid.cppm: kChunksPerEdge,
        // kChunkCount); the views span projects that resident set, so views
        // <= 1024 in practice. kSlots = 2048 = 2 x kChunkCount keeps the probe
        // load factor <= 0.5 on that path. The table is one 8 KiB i32 array on
        // the stack (-1 = empty, else the views ordinal): no heap allocation
        // for the index itself. A span beyond kSlots sets m_overflow and every
        // lookup falls back to the legacy linear scan — same results, slower.
        //
        // Equivalence: the table holds exactly the validated views set and is
        // lookup-only, never iterated for output; pending stays std::set, so
        // drain order, budget, neighbor fanout, and chain tracing are
        // untouched and the chains out are bit-identical for same views+dirty.
        struct ChunkIndex {
            static constexpr std::size_t kSlots = 2048u; // 2 x Sim kChunkCount (1024).
            static_assert((kSlots & (kSlots - 1u)) == 0u, "slots must stay a power of two for index masking");

            std::array<i32, kSlots> m_slots{};
            bool m_overflow{false};

            ChunkIndex() noexcept {
                m_slots.fill(-1);
            }

            [[nodiscard]] static u32 hash(const ColliderChunkPos pos) noexcept {
                const u32 x = static_cast<u32>(pos.m_x);
                const u32 y = static_cast<u32>(pos.m_y);
                return x * 0x9E3779B1u ^ y * 0x85EBCA6Bu;
            }

            [[nodiscard]] i32 find(const std::span<const ColliderChunkView> chunks,
                                   const ColliderChunkPos pos) const noexcept {
                if (m_overflow) {
                    const auto it = std::ranges::find(chunks, pos, &ColliderChunkView::m_pos);
                    return it == chunks.end() ? -1 : static_cast<i32>(it - chunks.begin());
                }
                std::size_t slot = hash(pos) & (kSlots - 1u);
                for (std::size_t n = 0; n < kSlots; ++n) {
                    const i32 entry = m_slots[slot];
                    if (entry < 0) {
                        return -1;
                    }
                    if (chunks[static_cast<std::size_t>(entry)].m_pos == pos) {
                        return entry;
                    }
                    slot = (slot + 1u) & (kSlots - 1u);
                }
                return -1;
            }

            [[nodiscard]] bool insert(const ColliderChunkPos pos, const std::size_t chunk) noexcept {
                std::size_t slot = hash(pos) & (kSlots - 1u);
                for (std::size_t n = 0; n < kSlots; ++n) {
                    if (m_slots[slot] < 0) {
                        m_slots[slot] = static_cast<i32>(chunk);
                        return true;
                    }
                    slot = (slot + 1u) & (kSlots - 1u);
                }
                return false;
            }

            // Duplicate check for views[upto]: indexed while the table holds
            // exactly views[0, upto), prefix-identical linear scan on overflow.
            [[nodiscard]] bool seenDuplicate(const std::span<const ColliderChunkView> chunks,
                                             const std::size_t upto) const noexcept {
                if (m_overflow) {
                    return std::ranges::find(chunks.begin(), chunks.begin() + upto, chunks[upto].m_pos,
                                             &ColliderChunkView::m_pos) != chunks.begin() + upto;
                }
                return find(chunks, chunks[upto].m_pos) >= 0;
            }

            void remember(const ColliderChunkPos pos, const std::size_t chunk) noexcept {
                if (m_overflow) {
                    return;
                }
                if (not insert(pos, chunk)) {
                    m_overflow = true;
                    m_slots.fill(-1);
                }
            }
        };

        [[nodiscard]] const ColliderChunkView *findChunk(const ChunkIndex &index,
                                                         const std::span<const ColliderChunkView> chunks,
                                                         const ColliderChunkPos pos) noexcept {
            const i32 entry = index.find(chunks, pos);
            return entry < 0 ? nullptr : &chunks[static_cast<std::size_t>(entry)];
        }

        [[nodiscard]] bool solid(const ChunkIndex &index, const std::span<const ColliderChunkView> chunks, const i32 edge,
                                 const i32 x, const i32 y) noexcept {
            const auto floorDiv = [edge](const i32 v) { return v >= 0 ? v / edge : (v + 1) / edge - 1; };
            const ColliderChunkPos pos{floorDiv(x), floorDiv(y)};
            const ColliderChunkView *const chunk = findChunk(index, chunks, pos);
            if (not chunk) {
                return false;
            }
            const i32 local_x = x - pos.m_x * edge;
            const i32 local_y = y - pos.m_y * edge;
            return chunk->m_elements[static_cast<std::size_t>(local_y) * edge + local_x] != 0u;
        }

        template<class F>
        void cellEdges(const ChunkIndex &index, const std::span<const ColliderChunkView> chunks, const i32 edge, const i32 x,
                       const i32 y, F &&append) {
            if (not solid(index, chunks, edge, x, y)) {
                return;
            }
            if (not solid(index, chunks, edge, x, y + 1)) {
                append(Edge{{x + 1, y + 1}, {x, y + 1}});
            }
            if (not solid(index, chunks, edge, x - 1, y)) {
                append(Edge{{x, y + 1}, {x, y}});
            }
            if (not solid(index, chunks, edge, x, y - 1)) {
                append(Edge{{x, y}, {x + 1, y}});
            }
            if (not solid(index, chunks, edge, x + 1, y)) {
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

        [[nodiscard]] std::optional<Edge> exteriorEdge(const ChunkIndex &index, const std::span<const ColliderChunkView> chunks,
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
                    cellEdges(index, chunks, edge, x, y, [&](const Edge candidate) {
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

        [[nodiscard]] std::vector<Polyline> extract(const ChunkIndex &index, const std::span<const ColliderChunkView> chunks,
                                                    const ColliderChunkPos pos, const i32 edge) {
            std::vector<Edge> edges;
            const std::size_t cells = static_cast<std::size_t>(edge) * static_cast<std::size_t>(edge);
            edges.reserve(2u * cells); // ~2 boundary edges per solid cell; sizes the walk output up front.
            const i32 origin_x = pos.m_x * edge;
            const i32 origin_y = pos.m_y * edge;
            for (i32 y = origin_y; y < origin_y + edge; ++y) {
                for (i32 x = origin_x; x < origin_x + edge; ++x) {
                    cellEdges(index, chunks, edge, x, y, [&](const Edge e) { edges.push_back(e); });
                }
            }
            // Chain-tracing endpoint index (H3b/Slice 6): one vector of
            // 2 x edges.size() ordinals — the single heap allocation on this
            // path — split into an outgoing half sorted by (from, ordinal)
            // and an incoming half sorted by (to, ordinal). next() scans only
            // the outgoing run for its vertex instead of all edges per step,
            // and has_predecessor() becomes a binary existence probe instead
            // of a full any_of per seed: tracing drops from O(E^2) to
            // O(E log E) for the one-time sorts plus O(degree) per step.
            //
            // Order-preservation proof: within an equal-vertex run the sort
            // tiebreaks by original ordinal, so iterating the outgoing run in
            // order visits exactly the edges the legacy linear scan would
            // match (from == vertex), in the same array order, applying the
            // same strict turnRank improvement (< keeps the earliest minimal
            // edge). Hence best and rank evolve identically and the returned
            // edge equals the linear scan's first minimal-rank match. The
            // incoming half holds every edge ordinal (used and unused, seed
            // included), so binary_search existence matches the legacy
            // any_of(e.to == seed.from) predicate bit-for-bit — the legacy
            // predicate never consulted used[]. Seed order, chain emission
            // order, and Polyline contents are untouched downstream.
            std::vector<std::size_t> endpointOrder;
            if (not edges.empty()) {
                endpointOrder.resize(2u * edges.size());
                const auto sortOutBegin = endpointOrder.begin();
                const auto sortOutEnd = sortOutBegin + static_cast<std::ptrdiff_t>(edges.size());
                const auto sortInEnd = endpointOrder.end();
                std::iota(sortOutBegin, sortOutEnd, 0u);
                std::iota(sortOutEnd, sortInEnd, 0u);
                std::sort(sortOutBegin, sortOutEnd, [&](const std::size_t a, const std::size_t b) {
                    const auto order = edges[a].from <=> edges[b].from;
                    return order != 0 ? order < 0 : a < b;
                });
                std::sort(sortOutEnd, sortInEnd, [&](const std::size_t a, const std::size_t b) {
                    const auto order = edges[a].to <=> edges[b].to;
                    return order != 0 ? order < 0 : a < b;
                });
            }
            std::vector<bool> used(edges.size());
            std::vector<Polyline> lines;
            const auto outBegin = endpointOrder.begin();
            const auto outEnd = outBegin + static_cast<std::ptrdiff_t>(edges.size());
            const auto fromOf = [&](const std::size_t i) -> const Vertex & { return edges[i].from; };
            const auto toOf = [&](const std::size_t i) -> const Vertex & { return edges[i].to; };
            const auto next = [&](const Vertex vertex, const Edge prior) -> std::size_t {
                std::size_t best = edges.size();
                int rank = 4;
                const auto runBegin = std::ranges::lower_bound(outBegin, outEnd, vertex, {}, fromOf);
                const auto runEnd = std::ranges::upper_bound(runBegin, outEnd, vertex, {}, fromOf);
                for (auto it = runBegin; it != runEnd; ++it) {
                    const std::size_t i = *it;
                    if (not used[i]) {
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
                    const bool has_predecessor =
                        std::ranges::binary_search(outEnd, endpointOrder.end(), edges[seed].from, {}, toOf);
                    if (phase == 0 and has_predecessor) {
                        continue;
                    }
                    Polyline line;
                    line.points.reserve(64u); // Seed for typical small loops; longer traces keep geometric growth.
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
                        const auto before = exteriorEdge(index, chunks, edge, pos, start, first, true);
                        const auto after = exteriorEdge(index, chunks, edge, pos, end, last, false);
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
        ChunkIndex index;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const auto &chunk = chunks[i];
            if (chunk.m_pos.m_x < 0 or
                chunk.m_pos.m_y < 0 or
                chunk.m_pos.m_x > 100000 / static_cast<i32>(edge) or
                chunk.m_pos.m_y > 100000 / static_cast<i32>(edge) or
                chunk.m_elements.size() != static_cast<std::size_t>(edge) * edge or
                index.seenDuplicate(chunks, i)) {
                return std::make_error_code(std::errc::invalid_argument);
            }
            index.remember(chunk.m_pos, i);
        }
        try {
            for (const ColliderChunkPos pos: dirty) {
                if (not findChunk(index, chunks, pos)) {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                for (const ColliderChunkPos adjacent: {
                         pos, ColliderChunkPos{pos.m_x - 1, pos.m_y},
                         ColliderChunkPos{pos.m_x + 1, pos.m_y}, ColliderChunkPos{pos.m_x, pos.m_y - 1},
                         ColliderChunkPos{pos.m_x, pos.m_y + 1}
                     }) {
                    if (findChunk(index, chunks, adjacent)) {
                        m_pending.insert(adjacent);
                    }
                }
            }
            while (not m_pending.empty() and processed < max_chunks) {
                const ColliderChunkPos pos = *m_pending.begin();
                const ColliderChunkView *const chunk = findChunk(index, chunks, pos);
                if (not chunk) {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                // H4b/Slice 6 order-preserving diff: extract the desired chains,
                // then reuse every slot whose payload is bit-identical to the
                // previous drain instead of destroy-all/recreate-all. Desired
                // geometry is a pure function of (views, edge), and the prior
                // payloads live in m_shapes, so exact float equality decides
                // keep vs replace per positional slot. New chains are created
                // in increasing line order (the legacy full-create order
                // restricted to added/changed slots), and the merged vector
                // keeps positional order — the resulting Scene chain set,
                // contents, and processing order match the legacy outcome
                // exactly; handles differ only where legacy also recreated.
                const auto sameGeometry = [](const Polyline &line, const StoredChain &shape) noexcept {
                    if (line.loop != shape.m_loop or
                        line.before.m_x != shape.m_before.m_x or
                        line.before.m_y != shape.m_before.m_y or
                        line.after.m_x != shape.m_after.m_x or
                        line.after.m_y != shape.m_after.m_y or
                        line.points.size() != shape.m_points.size()) {
                        return false;
                    }
                    for (std::size_t i = 0; i < line.points.size(); ++i) {
                        if (line.points[i].m_x != shape.m_points[i].m_x or
                            line.points[i].m_y != shape.m_points[i].m_y) {
                            return false;
                        }
                    }
                    return true;
                };

                std::vector<Polyline> lines;
                if (chunk->m_needed) {
                    lines = extract(index, chunks, pos, static_cast<i32>(edge));
                }
                auto existing = m_chains.find(pos);
                if (existing == m_chains.end() and lines.empty()) {
                    m_pending.erase(m_pending.begin());
                    ++processed;
                    continue;
                }
                const auto shapeIt = m_shapes.find(pos);
                const std::size_t oldCount = existing == m_chains.end() ? 0u : existing->second.size();
                const bool haveShapes = shapeIt != m_shapes.end() and shapeIt->second.size() == oldCount;
                const std::size_t common = std::min(oldCount, lines.size());
                std::vector<bool> keep(common, false);
                bool allKeep = haveShapes;
                for (std::size_t i = 0; i < common; ++i) {
                    keep[i] = haveShapes and sameGeometry(lines[i], shapeIt->second[i]);
                    allKeep = allKeep and keep[i];
                }
                if (existing != m_chains.end() and oldCount == lines.size() and allKeep) {
                    m_pending.erase(m_pending.begin());
                    ++processed;
                    continue;
                }

                // Snapshot the desired payloads before any Scene call so a
                // bad_alloc here has no Scene side effects (mirrors extract).
                std::vector<StoredChain> snapshot;
                snapshot.reserve(lines.size());
                for (const Polyline &line: lines) {
                    snapshot.push_back({line.points, line.before, line.after, line.loop});
                }

                // Preallocation before any Scene mutation: merged storage,
                // created storage, and the m_shapes map node for pos. Each may
                // throw bad_alloc here with zero Scene side effects; the
                // commit phase below then performs no allocation.
                std::vector<ChainHandle> merged;
                merged.reserve(lines.size());
                std::vector<std::pair<std::size_t, ChainHandle> > created;
                created.reserve(lines.size());
                const bool needShapeNode = not snapshot.empty() and shapeIt == m_shapes.end();
                auto shapeCommit = shapeIt;
                if (needShapeNode) {
                    shapeCommit = m_shapes.try_emplace(pos).first;
                }
                const bool shapePreinserted = needShapeNode;

                // Phase 1 (create-only): added slots plus changed slots, in
                // increasing line order. On failure only the chains created
                // above are destroyed, the preinserted shape node (if any) is
                // erased, and prior Scene/m_chains/m_shapes state is
                // untouched, exactly like the legacy create loop.
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (i < common and keep[i]) {
                        continue;
                    }
                    const Polyline &line = lines[i];
                    const ChainDefinition definition{line.points, line.before, line.after, 0u, 1u, line.loop};
                    auto result = scene.createChunkChain(definition);
                    if (not result) {
                        for (const auto &[_, handle]: created) {
                            (void) scene.destroyChunkChain(handle);
                        }
                        if (shapePreinserted) {
                            m_shapes.erase(shapeCommit);
                        }
                        return result.error();
                    }
                    created.emplace_back(i, *result);
                }

                // Phase 2 (destroy-only): removed slots plus replaced slots,
                // in increasing index order. On failure the phase-1 chains are
                // destroyed, the preinserted shape node (if any) is erased,
                // and m_chains/m_shapes are untouched, mirroring the legacy
                // destroy-failure rollback.
                if (existing == m_chains.end() and not created.empty()) {
                    try {
                        existing = m_chains.emplace(pos, std::vector<ChainHandle>{}).first;
                    } catch (...) {
                        for (const auto &[_, handle]: created) {
                            (void) scene.destroyChunkChain(handle);
                        }
                        if (shapePreinserted) {
                            m_shapes.erase(shapeCommit);
                        }
                        throw;
                    }
                }
                if (existing != m_chains.end()) {
                    for (std::size_t i = 0; i < oldCount; ++i) {
                        if (i < common and keep[i]) {
                            continue;
                        }
                        const std::error_code destroyed = scene.destroyChunkChain(existing->second[i]);
                        if (destroyed) {
                            for (const auto &[_, handle]: created) {
                                (void) scene.destroyChunkChain(handle);
                            }
                            if (shapePreinserted) {
                                m_shapes.erase(shapeCommit);
                            }
                            return destroyed;
                        }
                    }

                    // Phase 3 (splice, no Scene calls, no allocation): kept
                    // handles stay in their positional slots, created handles
                    // fill the rest — the merged vector equals the legacy
                    // fresh vector in contents and order. merged capacity was
                    // pre-reserved and ChainHandle push is noexcept, swap is
                    // noexcept, erases allocate nothing, and the shape commit
                    // is a vector move-assign stealing snapshot storage. The
                    // snapshot commits only here, on success, keeping m_shapes
                    // shadowing m_chains.
                    std::size_t next = 0u;
                    for (std::size_t i = 0; i < lines.size(); ++i) {
                        if (i < common and keep[i]) {
                            merged.push_back(existing->second[i]);
                        } else {
                            merged.push_back(created[next++].second);
                        }
                    }
                    existing->second.swap(merged);
                    if (existing->second.empty()) {
                        m_chains.erase(existing);
                    }
                    if (snapshot.empty()) {
                        m_shapes.erase(pos);
                    } else {
                        shapeCommit->second = std::move(snapshot);
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
