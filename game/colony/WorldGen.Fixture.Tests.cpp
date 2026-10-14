module;
#include "pP/UnitTest.h"
module game.tests;
import engine.core;
import engine.sim;
import game.colony.worldgen;
import std;
namespace pP::game::tests::detail::legacy_fixture {
    // ------------------------------------------------------------------
    // legacy world generation fixtures
    // ------------------------------------------------------------------

    using namespace sim;
    using colony::generateLegacyFixture;
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

        PPR_TEST_ASSERT(not generateLegacyFixture(1234567u, first));
        PPR_TEST_ASSERT(not generateLegacyFixture(1234567u, second));

        PPR_TEST_ASSERT(gridHash(first) == gridHash(second));

        ChunkGrid other{};
        PPR_TEST_ASSERT(not generateLegacyFixture(7654321u, other));

        PPR_TEST_ASSERT(gridHash(first) != gridHash(other));
    };

    PPR_UNIT_TEST (regen_from_seed_reproduces_grid) {
        ChunkGrid first{};
        PPR_TEST_ASSERT(not generateLegacyFixture(424242u, first));

        ChunkGrid fresh{};
        PPR_TEST_ASSERT(not generateLegacyFixture(424242u, fresh));

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

    PPR_UNIT_TEST (cancelled_generation_leaves_unstarted_grid_empty) {
        ChunkGrid grid{};
        const auto [context, cancel] = context::withCancel(context::background());
        cancel();
        std::atomic<u32> completed{};
        const GenerationExecution execution{.m_context = context.get(), .m_completed_chunks = &completed};
        PPR_TEST_ASSERT(generateLegacyFixture(42u, grid, &execution) == std::errc::operation_canceled);
        PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);
        PPR_TEST_ASSERT(completed.load() == 0u);
    };

    PPR_UNIT_TEST (fixed_seed_contains_every_archetype) {
        ChunkGrid grid{};
        PPR_TEST_ASSERT(not generateLegacyFixture(1234567u, grid));

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

        PPR_TEST_ASSERT(not generateLegacyFixture(99u, grid));

        PPR_TEST_ASSERT(grid.residentChunkCount() == kChunkCount);
        PPR_TEST_ASSERT(grid.dirtyChunkCount() == kChunkCount);
        PPR_TEST_ASSERT(grid.dirtyChunks().size() == kChunkCount);
    };

    PPR_UNIT_TEST (generation_content_pinned) {
        ChunkGrid grid{};
        PPR_TEST_ASSERT(not generateLegacyFixture(1234567u, grid));

        constexpr u64 kPinnedHash = 12692632048894322839u;
        PPR_TEST_ASSERT(gridHash(grid) == kPinnedHash);
    };
}

namespace pP::game::tests {
    const UnitTest legacy_fixture = UnitTest::Named("legacy_fixture") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::legacy_fixture::same_seed_generates_identical_grid,
                detail::legacy_fixture::regen_from_seed_reproduces_grid,
                detail::legacy_fixture::cancelled_generation_leaves_unstarted_grid_empty,
                detail::legacy_fixture::fixed_seed_contains_every_archetype,
                detail::legacy_fixture::generation_marks_every_chunk_dirty,
                detail::legacy_fixture::generation_content_pinned});
    };
    const UnitTest &gameLegacyFixtureTests() noexcept {
        return legacy_fixture;
    }
}
