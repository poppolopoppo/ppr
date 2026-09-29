module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    namespace Grid {
        using namespace pP::sim;

        PPR_UNIT_TEST (world_geometry_constants) {
            PPR_TEST_ASSERT(kChunkEdge == 128u);
            PPR_TEST_ASSERT(kChunksPerEdge == 32u);
            PPR_TEST_ASSERT(kWorldEdge == 4096u);
            PPR_TEST_ASSERT(kChunkCount == 1024u);
            PPR_TEST_ASSERT(kCellsPerChunk == 16384u);

            PPR_TEST_ASSERT(kWorldEdge == kChunkEdge * kChunksPerEdge);
            PPR_TEST_ASSERT(kChunkCount == kChunksPerEdge * kChunksPerEdge);
            PPR_TEST_ASSERT(kCellsPerChunk == kChunkEdge * kChunkEdge);
        };

        PPR_UNIT_TEST (coordinate_conversions) {
            constexpr GlobalCellPos corner{4095u, 4095u};
            constexpr GlobalCellPos origin{0u, 0u};

            PPR_TEST_ASSERT(isInsideWorld(corner));
            PPR_TEST_ASSERT(isInsideWorld(origin));
            PPR_TEST_ASSERT(not isInsideWorld(GlobalCellPos{kWorldEdge, 0u}));
            PPR_TEST_ASSERT(not isInsideWorld(GlobalCellPos{0u, kWorldEdge}));

            PPR_TEST_ASSERT(chunkOf(corner) == (ChunkPos{31u, 31u}));
            PPR_TEST_ASSERT(localOf(corner) == (LocalCellPos{127u, 127u}));
            PPR_TEST_ASSERT(chunkIndexOf(ChunkPos{31u, 31u}) == 1023u);
            PPR_TEST_ASSERT(chunkIndexOf(ChunkPos{}) == 0u);

            PPR_TEST_ASSERT(globalOf(chunkOf(corner), localOf(corner)) == corner);
            PPR_TEST_ASSERT(globalOf(ChunkPos{1u, 2u}, LocalCellPos{3u, 4u}) == (GlobalCellPos{131u, 260u}));

            PPR_TEST_ASSERT(isInsideChunkGrid(ChunkPos{31u, 31u}));
            PPR_TEST_ASSERT(not isInsideChunkGrid(ChunkPos{32u, 0u}));
            PPR_TEST_ASSERT(isInsideChunk(LocalCellPos{127u, 127u}));
            PPR_TEST_ASSERT(not isInsideChunk(LocalCellPos{128u, 0u}));
        };

        PPR_UNIT_TEST (write_then_read_cell) {
            ChunkGrid grid{};

            const GlobalCellPos pos{10u, 20u};
            PPR_TEST_ASSERT(not grid.setCell(pos, Cell{7u, 350.0f}));

            const Expected<Cell> cell = grid.getCell(pos);
            PPR_TEST_ASSERT(cell.has_value());
            PPR_TEST_ASSERT(*cell == (Cell{7u, 350.0f}));

            PPR_TEST_ASSERT(grid.isResident(chunkOf(pos)));
            PPR_TEST_ASSERT(grid.residentChunkCount() == 1u);
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 1u);
        };

        PPR_UNIT_TEST (absent_chunk_reads_default_cell) {
            const ChunkGrid grid{};

            PPR_TEST_ASSERT(not grid.isResident(ChunkPos{5u, 9u}));
            PPR_TEST_ASSERT(not grid.residentCells(ChunkPos{5u, 9u}).has_value());

            const Expected<Cell> cell = grid.getCell(globalOf(ChunkPos{5u, 9u}, LocalCellPos{1u, 1u}));
            PPR_TEST_ASSERT(cell.has_value());
            PPR_TEST_ASSERT(*cell == Cell{});
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
        };

        PPR_UNIT_TEST (resident_payload_is_a_full_chunk) {
            ChunkGrid grid{};
            const ChunkPos chunk{3u, 7u};
            const GlobalCellPos pos = globalOf(chunk, LocalCellPos{1u, 2u});

            PPR_TEST_ASSERT(not grid.setCell(pos, Cell{5u, 300.0f}));

            const std::optional<std::span<const Cell> > cells = grid.residentCells(chunk);
            PPR_TEST_ASSERT(cells.has_value());
            PPR_TEST_ASSERT(cells->size() == kCellsPerChunk);
            PPR_TEST_ASSERT((*cells)[2u * kChunkEdge + 1u] == (Cell{5u, 300.0f}));
        };

        PPR_UNIT_TEST (rejects_invalid_cells) {
            ChunkGrid grid{};

            const GlobalCellPos outside{kWorldEdge, 0u};
            PPR_TEST_ASSERT(grid.setCell(outside, Cell{}) == std::make_error_code(std::errc::result_out_of_range));

            const GlobalCellPos inside{1u, 1u};
            const Cell nan_cell{9u, std::numeric_limits<float>::quiet_NaN()};
            PPR_TEST_ASSERT(grid.setCell(inside, nan_cell) == std::make_error_code(std::errc::invalid_argument));

            // Infinity is non-finite too: rejected exactly like NaN.
            const Cell inf_cell{9u, std::numeric_limits<float>::infinity()};
            PPR_TEST_ASSERT(grid.setCell(inside, inf_cell) == std::make_error_code(std::errc::invalid_argument));

            // Neither rejection mutated the grid.
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);

            const Expected<Cell> untouched = grid.getCell(inside);
            PPR_TEST_ASSERT(untouched.has_value());
            PPR_TEST_ASSERT(*untouched == Cell{});

            const Expected<Cell> out_of_world = grid.getCell(outside);
            PPR_TEST_ASSERT(not out_of_world.has_value());
            PPR_TEST_ASSERT(out_of_world.error() == std::make_error_code(std::errc::result_out_of_range));
        };

        PPR_UNIT_TEST (mark_dirty_implies_residency) {
            ChunkGrid grid{};
            const ChunkPos chunk{2u, 3u};

            PPR_TEST_ASSERT(not grid.isResident(chunk));

            grid.markDirty(chunk);

            PPR_TEST_ASSERT(grid.isResident(chunk));
            PPR_TEST_ASSERT(grid.residentChunkCount() == 1u);
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 1u);
            PPR_TEST_ASSERT(grid.activity(chunk) == EChunkActivity::active);
        };

        PPR_UNIT_TEST (dirty_and_activity_queries) {
            ChunkGrid grid{};
            const ChunkPos first{1u, 2u};
            const ChunkPos second{4u, 5u};

            grid.markDirty(first);
            grid.markDirty(second);
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 2u);

            grid.setActivity(second, EChunkActivity::quiescent);

            const Array<ChunkPos> active = grid.activeDirtyChunks();
            PPR_TEST_ASSERT(active.size() == 1u);
            PPR_TEST_ASSERT(active[0] == first);
            PPR_TEST_ASSERT(grid.dirtyChunks().size() == 2u);
            PPR_TEST_ASSERT(grid.activity(second) == EChunkActivity::quiescent);

            grid.clearDirty(first);
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 1u);
            PPR_TEST_ASSERT(grid.activeDirtyChunks().empty());

            grid.clearAllDirty();
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.dirtyChunks().empty());
        };
    } // namespace Grid
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest chunk_grid = UnitTest::Named("chunk_grid") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Grid::world_geometry_constants,
            detail::Grid::coordinate_conversions,
            detail::Grid::write_then_read_cell,
            detail::Grid::absent_chunk_reads_default_cell,
            detail::Grid::resident_payload_is_a_full_chunk,
            detail::Grid::rejects_invalid_cells,
            detail::Grid::mark_dirty_implies_residency,
            detail::Grid::dirty_and_activity_queries,
        });
    };

    const UnitTest &chunkGridTests() noexcept {
        return chunk_grid;
    }
} // namespace pP::tests
