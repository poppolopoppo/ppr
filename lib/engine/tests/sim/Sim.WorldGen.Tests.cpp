module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    namespace WorldGen {
        using namespace pP::sim;

        // Element ids below are numeric literals matching the colony-owned
        // registry (`game/colony/Colony.Elements.h`): the sim test target must
        // not depend on game code, so the mapping is documented, not included.
        // 0 vacuum, 1 rock, 2 ice, 3 caustic, 4 oil, 5 ruins, 6 abyssal, 7 vent.

        [[nodiscard]] u64 gridHash(const ChunkGrid &grid) {
            u64 hash = 14695981039346656037ULL;
            for (u32 chunk_y = 0u; chunk_y < kChunksPerEdge; ++chunk_y) {
                for (u32 chunk_x = 0u; chunk_x < kChunksPerEdge; ++chunk_x) {
                    const auto cells = grid.residentCells(ChunkPos{chunk_x, chunk_y});
                    PPR_TEST_ASSERT(cells.has_value());
                    for (const Cell &cell: *cells) {
                        // Single FNV-1a round over the packed (element,
                        // temperature-bits) payload: the same
                        // equality/inequality discrimination at half the
                        // multiplies per cell.
                        const u64 packed = (static_cast<u64>(cell.m_element) << 32u) |
                                           std::bit_cast<u32>(cell.m_temperature);
                        hash ^= packed;
                        hash *= 1099511628211ULL;
                    }
                }
            }

            return hash;
        }

        PPR_UNIT_TEST (same_seed_generates_identical_grid) {
            ChunkGrid first{};
            ChunkGrid second{};

            generate(1234567u, first);
            generate(1234567u, second);

            PPR_TEST_ASSERT(gridHash(first) == gridHash(second));

            ChunkGrid other{};
            generate(7654321u, other);

            PPR_TEST_ASSERT(gridHash(first) != gridHash(other));
        };

        PPR_UNIT_TEST (regen_from_seed_reproduces_grid) {
            ChunkGrid first{};
            generate(424242u, first);

            ChunkGrid fresh{};
            generate(424242u, fresh);

            for (u32 chunk_y = 0u; chunk_y < kChunksPerEdge; ++chunk_y) {
                for (u32 chunk_x = 0u; chunk_x < kChunksPerEdge; ++chunk_x) {
                    const ChunkPos chunk{chunk_x, chunk_y};
                    const auto expected = first.residentCells(chunk);
                    const auto actual = fresh.residentCells(chunk);
                    PPR_TEST_ASSERT(expected.has_value() and actual.has_value());
                    PPR_TEST_ASSERT(std::ranges::equal(*expected, *actual));
                }
            }
        };

        PPR_UNIT_TEST (fixed_seed_contains_every_archetype) {
            ChunkGrid grid{};
            generate(1234567u, grid);

            std::array<u64, 8u> counts{};
            bool cold_frost = false;
            bool hot_pocket = false;
            bool hot_vent = false;
            bool mild_base = false;

            for (u32 chunk_y = 0u; chunk_y < kChunksPerEdge; ++chunk_y) {
                for (u32 chunk_x = 0u; chunk_x < kChunksPerEdge; ++chunk_x) {
                    const auto cells = grid.residentCells(ChunkPos{chunk_x, chunk_y});
                    PPR_TEST_ASSERT(cells.has_value());
                    for (const Cell &cell: *cells) {
                        if (cell.m_element < static_cast<u16>(counts.size())) {
                            ++counts[cell.m_element];
                        }
                        const float temp = cell.m_temperature;
                        PPR_TEST_ASSERT(std::isfinite(temp));
                        if (cell.m_element == 2u and temp < 240.0f) {
                            cold_frost = true;
                        }
                        if (cell.m_element == 3u and temp > 320.0f) {
                            hot_pocket = true;
                        }
                        if (cell.m_element == 7u and temp > 380.0f) {
                            hot_vent = true;
                        }
                        if (cell.m_element == 1u and temp >= 285.0f and temp <= 300.0f) {
                            mild_base = true;
                        }
                    }
                }
            }

            // Presence, not exact layout: geometric cores guarantee every
            // archetype for any seed, so these minimums survive tuning.
            PPR_TEST_ASSERT(counts[0] > 10000u);
            PPR_TEST_ASSERT(counts[1] > counts[0]);
            PPR_TEST_ASSERT(counts[2] > 100000u);
            PPR_TEST_ASSERT(counts[3] > 10000u);
            PPR_TEST_ASSERT(counts[4] > 5000u);
            PPR_TEST_ASSERT(counts[5] > 10000u);
            PPR_TEST_ASSERT(counts[6] > 100000u);
            PPR_TEST_ASSERT(counts[7] > 100u);

            PPR_TEST_ASSERT(cold_frost);
            PPR_TEST_ASSERT(hot_pocket);
            PPR_TEST_ASSERT(hot_vent);
            PPR_TEST_ASSERT(mild_base);
        };

        PPR_UNIT_TEST (generation_marks_every_chunk_dirty) {
            ChunkGrid grid{};
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);

            generate(99u, grid);

            PPR_TEST_ASSERT(grid.residentChunkCount() == kChunkCount);
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == kChunkCount);
            PPR_TEST_ASSERT(grid.dirtyChunks().size() == kChunkCount);
        };
    } // namespace WorldGen
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest world_gen = UnitTest::Named("world_gen") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::WorldGen::same_seed_generates_identical_grid,
            detail::WorldGen::regen_from_seed_reproduces_grid,
            detail::WorldGen::fixed_seed_contains_every_archetype,
            detail::WorldGen::generation_marks_every_chunk_dirty,
        });
    };

    const UnitTest &worldGenTests() noexcept {
        return world_gen;
    }
} // namespace pP::tests
