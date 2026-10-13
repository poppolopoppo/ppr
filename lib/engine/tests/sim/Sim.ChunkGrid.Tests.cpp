module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // chunk grid
    // ------------------------------------------------------------------

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

            const std::optional<std::span<const Cell>> cells = grid.residentCells(chunk);
            PPR_TEST_ASSERT(cells.has_value());
            PPR_TEST_ASSERT(cells->size() == kCellsPerChunk);
            PPR_TEST_ASSERT((*cells)[2u * kChunkEdge + 1u] == (Cell{5u, 300.0f}));
        };

        PPR_UNIT_TEST (retained_payload_tracks_residency_without_counting_capture_owners) {
            ChunkGrid grid{};
            const u64 empty_bytes = grid.retainedPayloadBytes();
            PPR_TEST_ASSERT(empty_bytes > 0u);
            PPR_TEST_ASSERT(not grid.setCell({1u, 2u}, {17u, 293.0f}));
            const u64 resident_bytes = grid.retainedPayloadBytes();
            PPR_TEST_ASSERT(resident_bytes >= empty_bytes + static_cast<u64>(kCellsPerChunk) * sizeof(Cell));
            auto capture = grid.captureChunk({0u, 0u});
            PPR_TEST_ASSERT(capture.has_value());
            PPR_TEST_ASSERT(grid.retainedPayloadBytes() == resident_bytes);
            grid.clearAllDirty();
            PPR_TEST_ASSERT(grid.retainedPayloadBytes() == resident_bytes);
            PPR_TEST_ASSERT(not grid.setCell({129u, 2u}, {17u, 293.0f}));
            PPR_TEST_ASSERT(
                grid.retainedPayloadBytes() >= resident_bytes + static_cast<u64>(kCellsPerChunk) * sizeof(Cell));
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

        PPR_UNIT_TEST (dirty_bounds_grow_with_writes) {
            ChunkGrid grid{};
            const ChunkPos chunk{};

            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());

            PPR_TEST_ASSERT(not grid.setCell(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}));
            const std::optional<DirtyBounds> single = grid.dirtyBounds(chunk);
            PPR_TEST_ASSERT(single.has_value());
            PPR_TEST_ASSERT(*single == (DirtyBounds{10u, 20u, 10u, 20u}));
            PPR_TEST_ASSERT(single->width() == 1u);
            PPR_TEST_ASSERT(single->height() == 1u);

            PPR_TEST_ASSERT(not grid.setCell(GlobalCellPos{12u, 21u}, Cell{7u, 44.0f}));
            const std::optional<DirtyBounds> grown = grid.dirtyBounds(chunk);
            PPR_TEST_ASSERT(grown.has_value());
            PPR_TEST_ASSERT(*grown == (DirtyBounds{10u, 20u, 12u, 21u}));
            PPR_TEST_ASSERT(grown->width() == 3u);
            PPR_TEST_ASSERT(grown->height() == 2u);

            // A write inside the box keeps the superset; other chunks stay clean.
            PPR_TEST_ASSERT(not grid.setCell(GlobalCellPos{11u, 20u}, Cell{5u, 13.5f}));
            PPR_TEST_ASSERT(grid.dirtyBounds(chunk) == grown);
            PPR_TEST_ASSERT(not grid.dirtyBounds(ChunkPos{1u, 0u}).has_value());

            grid.clearDirty(chunk);
            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);

            PPR_TEST_ASSERT(not grid.setCell(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}));
            PPR_TEST_ASSERT(grid.dirtyBounds(chunk) == single);
            grid.clearAllDirty();
            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());
        };

        PPR_UNIT_TEST (mark_dirty_sets_full_bounds) {
            ChunkGrid grid{};
            const ChunkPos chunk{4u, 6u};

            grid.markDirty(chunk);
            const std::optional<DirtyBounds> full = grid.dirtyBounds(chunk);
            PPR_TEST_ASSERT(full.has_value());
            PPR_TEST_ASSERT(*full == (DirtyBounds{0u, 0u, kChunkEdge - 1u, kChunkEdge - 1u}));
            PPR_TEST_ASSERT(full->width() == kChunkEdge);
            PPR_TEST_ASSERT(full->height() == kChunkEdge);

            grid.clearDirty(chunk);
            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());
        };

        PPR_UNIT_TEST (setCells_rejects_wrong_size_without_mutation) {
            ChunkGrid grid{};
            const ChunkPos chunk{1u, 2u};

            Array<Cell> empty{};
            PPR_TEST_ASSERT(
                grid.setCells(chunk, std::span<const Cell>{empty}) ==
                std::make_error_code(std::errc::invalid_argument));

            Array<Cell> short_cells{};
            short_cells.resize(kCellsPerChunk - 1u);
            PPR_TEST_ASSERT(
                grid.setCells(chunk, std::span<const Cell>{short_cells}) ==
                std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(not grid.isResident(chunk));
            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);
        };

        PPR_UNIT_TEST (setCells_rejects_non_finite_without_mutation) {
            ChunkGrid grid{};
            const ChunkPos chunk{3u, 4u};

            Array<Cell> cells{};
            cells.resize(kCellsPerChunk, Cell{1u, 293.0f});
            cells.back().m_temperature = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(
                grid.setCells(chunk, std::span<const Cell>{cells}) ==
                std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(not grid.isResident(chunk));
            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);

            // A resident payload is left untouched by a failed bulk write.
            Array<Cell> good{};
            good.resize(kCellsPerChunk, Cell{4u, 300.0f});
            PPR_TEST_ASSERT(not grid.setCells(chunk, std::span<const Cell>{good}));

            Array<Cell> bad = good;
            bad.back().m_temperature = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(
                grid.setCells(chunk, std::span<const Cell>{bad}) == std::make_error_code(std::errc::invalid_argument));

            const std::optional<std::span<const Cell>> payload = grid.residentCells(chunk);
            PPR_TEST_ASSERT(payload.has_value());
            PPR_TEST_ASSERT(std::ranges::equal(*payload, good));
        };

        PPR_UNIT_TEST (setCells_writes_and_marks_full_bounds) {
            ChunkGrid grid{};
            const ChunkPos chunk{2u, 1u};

            Array<Cell> cells{};
            cells.resize(kCellsPerChunk);
            for (u32 index = 0u; index < kCellsPerChunk; ++index) {
                cells[index] = Cell{static_cast<u16>(index % 7u), 200.0f + static_cast<float>(index % 100u) * 0.5f};
            }
            PPR_TEST_ASSERT(not grid.setCells(chunk, std::span<const Cell>{cells}));

            PPR_TEST_ASSERT(grid.isResident(chunk));
            PPR_TEST_ASSERT(grid.residentChunkCount() == 1u);
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 1u);

            const std::optional<DirtyBounds> full = grid.dirtyBounds(chunk);
            PPR_TEST_ASSERT(full.has_value());
            PPR_TEST_ASSERT(*full == (DirtyBounds{0u, 0u, kChunkEdge - 1u, kChunkEdge - 1u}));

            const GlobalCellPos first = globalOf(chunk, LocalCellPos{0u, 0u});
            const Expected<Cell> first_cell = grid.getCell(first);
            PPR_TEST_ASSERT(first_cell.has_value());
            PPR_TEST_ASSERT(*first_cell == cells[0u]);

            const GlobalCellPos last = globalOf(chunk, LocalCellPos{kChunkEdge - 1u, kChunkEdge - 1u});
            const Expected<Cell> last_cell = grid.getCell(last);
            PPR_TEST_ASSERT(last_cell.has_value());
            PPR_TEST_ASSERT(*last_cell == cells[kCellsPerChunk - 1u]);
        };

        PPR_UNIT_TEST (captured_chunk_restores_exact_state_and_exchanges_owned_payload) {
            ChunkGrid grid{};
            const ChunkPos pos{1u, 2u};
            const GlobalCellPos cell = globalOf(pos, {3u, 4u});
            PPR_TEST_ASSERT(not grid.setCell(cell, Cell{257u, 300.0f}));
            grid.setActivity(pos, EChunkActivity::quiescent);
            const auto bounds = grid.dirtyBounds(pos);
            auto prior = grid.captureChunk(pos);
            PPR_TEST_ASSERT(prior.has_value() and prior->position() == pos);
            PPR_TEST_ASSERT(prior->residentCells()->data() != grid.residentCells(pos)->data());
            PPR_TEST_ASSERT(not grid.setCell(globalOf(pos, {100u, 110u}), Cell{12u, 400.0f}));
            grid.setActivity(pos, EChunkActivity::active);
            const Cell *const candidate_storage = grid.residentCells(pos)->data();

            grid.exchangeChunk(*prior);
            PPR_TEST_ASSERT(grid.dirtyBounds(pos) == bounds);
            PPR_TEST_ASSERT(grid.activity(pos) == EChunkActivity::quiescent);
            PPR_TEST_ASSERT(*grid.getCell(cell) == (Cell{257u, 300.0f}));
            PPR_TEST_ASSERT(*grid.getCell(globalOf(pos, {100u, 110u})) == Cell{});
            PPR_TEST_ASSERT(prior->residentCells()->data() == candidate_storage);
            auto candidate = std::move(*prior);
            grid.exchangeChunk(candidate);
            PPR_TEST_ASSERT(grid.residentCells(pos)->data() == candidate_storage);
            PPR_TEST_ASSERT(grid.activity(pos) == EChunkActivity::active);
            PPR_TEST_ASSERT(*grid.getCell(globalOf(pos, {100u, 110u})) == (Cell{12u, 400.0f}));
        };

        PPR_UNIT_TEST (captured_chunk_can_restore_clean_nonresidency_without_mutating_other_chunks) {
            ChunkGrid grid{};
            const ChunkPos pos{3u, 4u};
            auto prior = grid.captureChunk(pos);
            PPR_TEST_ASSERT(prior.has_value() and not prior->residentCells());
            PPR_TEST_ASSERT(not grid.setCell({1u, 1u}, Cell{7u, 310.0f}));
            const auto other_bounds = grid.dirtyBounds({0u, 0u});
            PPR_TEST_ASSERT(not grid.setCell(globalOf(pos, {1u, 1u}), Cell{12u, 280.0f}));
            grid.exchangeChunk(*prior);
            PPR_TEST_ASSERT(not grid.isResident(pos) and not grid.dirtyBounds(pos));
            PPR_TEST_ASSERT(grid.residentChunkCount() == 1u);
            PPR_TEST_ASSERT(grid.dirtyBounds({0u, 0u}) == other_bounds);
            PPR_TEST_ASSERT(*grid.getCell({1u, 1u}) == (Cell{7u, 310.0f}));
            grid.exchangeChunk(*prior);
            PPR_TEST_ASSERT(grid.isResident(pos) and grid.dirtyBounds(pos).has_value());
            const auto invalid = grid.captureChunk({kChunksPerEdge, 0u});
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::result_out_of_range);
        };

        PPR_UNIT_TEST (adopt_rejects_wrong_size_without_mutation) {
            ChunkGrid grid{};
            const ChunkPos chunk{1u, 2u};

            Array<Cell> empty{};
            PPR_TEST_ASSERT(
                grid.setCells(chunk, std::move(empty)) == std::make_error_code(std::errc::invalid_argument));

            Array<Cell> short_cells{};
            short_cells.resize(kCellsPerChunk - 1u);
            PPR_TEST_ASSERT(
                grid.setCells(chunk, std::move(short_cells)) == std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(not grid.isResident(chunk));
            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);
        };

        PPR_UNIT_TEST (adopt_rejects_non_finite_without_mutation) {
            ChunkGrid grid{};
            const ChunkPos chunk{3u, 4u};

            Array<Cell> cells{};
            cells.resize(kCellsPerChunk, Cell{1u, 293.0f});
            cells.back().m_temperature = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(
                grid.setCells(chunk, std::move(cells)) == std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(not grid.isResident(chunk));
            PPR_TEST_ASSERT(not grid.dirtyBounds(chunk).has_value());
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);

            // A resident payload is left untouched by a failed adopting write.
            Array<Cell> good{};
            good.resize(kCellsPerChunk, Cell{4u, 300.0f});
            const Array<Cell> expected = good;
            PPR_TEST_ASSERT(not grid.setCells(chunk, std::move(good)));

            Array<Cell> bad = expected;
            bad.back().m_temperature = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(grid.setCells(chunk, std::move(bad)) == std::make_error_code(std::errc::invalid_argument));

            const std::optional<std::span<const Cell>> payload = grid.residentCells(chunk);
            PPR_TEST_ASSERT(payload.has_value());
            PPR_TEST_ASSERT(std::ranges::equal(*payload, expected));
        };

        PPR_UNIT_TEST (adopt_moves_payload_and_marks_full_bounds) {
            ChunkGrid grid{};
            const ChunkPos chunk{2u, 1u};

            Array<Cell> cells{};
            cells.resize(kCellsPerChunk);
            for (u32 index = 0u; index < kCellsPerChunk; ++index) {
                cells[index] = Cell{static_cast<u16>(index % 7u), 200.0f + static_cast<float>(index % 100u) * 0.5f};
            }
            const Array<Cell> expected = cells;
            PPR_TEST_ASSERT(not grid.setCells(chunk, std::move(cells)));

            PPR_TEST_ASSERT(grid.isResident(chunk));
            PPR_TEST_ASSERT(grid.residentChunkCount() == 1u);
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 1u);

            const std::optional<DirtyBounds> full = grid.dirtyBounds(chunk);
            PPR_TEST_ASSERT(full.has_value());
            PPR_TEST_ASSERT(*full == (DirtyBounds{0u, 0u, kChunkEdge - 1u, kChunkEdge - 1u}));

            const GlobalCellPos first = globalOf(chunk, LocalCellPos{0u, 0u});
            const Expected<Cell> first_cell = grid.getCell(first);
            PPR_TEST_ASSERT(first_cell.has_value());
            PPR_TEST_ASSERT(*first_cell == expected[0u]);

            const GlobalCellPos last = globalOf(chunk, LocalCellPos{kChunkEdge - 1u, kChunkEdge - 1u});
            const Expected<Cell> last_cell = grid.getCell(last);
            PPR_TEST_ASSERT(last_cell.has_value());
            PPR_TEST_ASSERT(*last_cell == expected[kCellsPerChunk - 1u]);
        };
    } // namespace Grid
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest chunk_grid = UnitTest::Named("chunk_grid") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Grid::retained_payload_tracks_residency_without_counting_capture_owners,
            detail::Grid::world_geometry_constants,
            detail::Grid::coordinate_conversions,
            detail::Grid::write_then_read_cell,
            detail::Grid::absent_chunk_reads_default_cell,
            detail::Grid::resident_payload_is_a_full_chunk,
            detail::Grid::rejects_invalid_cells,
            detail::Grid::mark_dirty_implies_residency,
            detail::Grid::dirty_and_activity_queries,
            detail::Grid::dirty_bounds_grow_with_writes,
            detail::Grid::mark_dirty_sets_full_bounds,
            detail::Grid::setCells_rejects_wrong_size_without_mutation,
            detail::Grid::setCells_rejects_non_finite_without_mutation,
            detail::Grid::setCells_writes_and_marks_full_bounds,
            detail::Grid::captured_chunk_restores_exact_state_and_exchanges_owned_payload,
            detail::Grid::captured_chunk_can_restore_clean_nonresidency_without_mutating_other_chunks,
            detail::Grid::adopt_rejects_wrong_size_without_mutation,
            detail::Grid::adopt_rejects_non_finite_without_mutation,
            detail::Grid::adopt_moves_payload_and_marks_full_bounds,
        });
    };

    const UnitTest &chunkGridTests() noexcept {
        return chunk_grid;
    }
} // namespace pP::tests
