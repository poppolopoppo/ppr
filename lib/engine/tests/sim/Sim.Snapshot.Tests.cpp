module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    namespace SnapshotSuite {
        using namespace pP::sim;

        /// Little-endian u32 at `offset`, mirroring the `save` layout.
        [[nodiscard]] u32 readU32(const std::span<const u8> bytes, const std::size_t offset) {
            u32 value = 0u;
            for (std::size_t index = 0u; index < 4u; ++index) {
                value |= static_cast<u32>(bytes[offset + index]) << (index * 8u);
            }

            return value;
        }

        [[nodiscard]] Snapshot singleCellSnapshot(const GlobalCellPos pos, const Cell cell, const u64 seed) {
            ChunkGrid grid{};
            PPR_TEST_ASSERT(not grid.setCell(pos, cell));

            return capture(grid, seed);
        }

        PPR_UNIT_TEST (capture_reads_dirty_chunks) {
            ChunkGrid grid{};
            const GlobalCellPos pos{10u, 20u};
            PPR_TEST_ASSERT(not grid.setCell(pos, Cell{3u, 12.5f}));

            const Snapshot snapshot = capture(grid, 42u);

            PPR_TEST_ASSERT(snapshot.m_version == kSnapshotVersion);
            PPR_TEST_ASSERT(snapshot.m_seed == 42u);
            PPR_TEST_ASSERT(snapshot.m_deltas.size() == 1u);
            PPR_TEST_ASSERT(snapshot.m_deltas[0].m_chunk == chunkIndexOf(chunkOf(pos)));
            PPR_TEST_ASSERT(snapshot.m_deltas[0].m_cells.size() == kCellsPerChunk);
            PPR_TEST_ASSERT(snapshot.m_deltas[0].m_cells[20u * kChunkEdge + 10u] == (Cell{3u, 12.5f}));
            PPR_TEST_ASSERT(snapshot.m_streams.empty());

            // Observational: the grid keeps its dirty state.
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 1u);
        };

        PPR_UNIT_TEST (capture_orders_two_dirty_chunks_row_major) {
            ChunkGrid grid{};

            // Inserted in reverse row-major order: capture must not follow
            // insertion order but the row-major chunk index order.
            const ChunkPos later{5u, 2u};
            const ChunkPos earlier{1u, 0u};
            PPR_TEST_ASSERT(not grid.setCell(globalOf(later, LocalCellPos{1u, 2u}), Cell{7u, 300.0f}));
            PPR_TEST_ASSERT(not grid.setCell(globalOf(earlier, LocalCellPos{3u, 4u}), Cell{9u, 310.0f}));

            const Snapshot snapshot = capture(grid, 5u);

            PPR_TEST_ASSERT(snapshot.m_deltas.size() == 2u);
            PPR_TEST_ASSERT(snapshot.m_deltas[0].m_chunk == chunkIndexOf(earlier));
            PPR_TEST_ASSERT(snapshot.m_deltas[1].m_chunk == chunkIndexOf(later));
            PPR_TEST_ASSERT(snapshot.m_deltas[0].m_cells[4u * kChunkEdge + 3u] == (Cell{9u, 310.0f}));
            PPR_TEST_ASSERT(snapshot.m_deltas[1].m_cells[2u * kChunkEdge + 1u] == (Cell{7u, 300.0f}));
        };

        PPR_UNIT_TEST (capture_of_marked_dirty_chunk_is_zero_filled) {
            ChunkGrid grid{};
            const ChunkPos chunk{4u, 6u};

            // markDirty only: no cell was ever written, yet the chunk is dense.
            grid.markDirty(chunk);

            const Snapshot snapshot = capture(grid, 1u);

            PPR_TEST_ASSERT(snapshot.m_deltas.size() == 1u);
            PPR_TEST_ASSERT(snapshot.m_deltas[0].m_chunk == chunkIndexOf(chunk));
            PPR_TEST_ASSERT(snapshot.m_deltas[0].m_cells.size() == kCellsPerChunk);

            for (const Cell &cell: snapshot.m_deltas[0].m_cells) {
                PPR_TEST_ASSERT(cell == Cell{});
            }
        };

        PPR_UNIT_TEST (capture_of_clean_grid_is_empty) {
            const ChunkGrid grid{};

            const Snapshot snapshot = capture(grid, 1u);
            PPR_TEST_ASSERT(snapshot.m_deltas.empty());

            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());

            const Expected<Snapshot> loaded = load(std::span<const u8>{saved.value()});
            PPR_TEST_ASSERT(loaded.has_value());
            PPR_TEST_ASSERT(*loaded == snapshot);
        };

        PPR_UNIT_TEST (save_load_round_trip) {
            constexpr u64 kSeed = 0x0123456789ABCDEFull;
            const Snapshot snapshot = singleCellSnapshot(GlobalCellPos{3u, 4u}, Cell{11u, 271.5f}, kSeed);

            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());

            const Expected<Snapshot> loaded = load(std::span<const u8>{saved.value()});
            PPR_TEST_ASSERT(loaded.has_value());
            PPR_TEST_ASSERT(*loaded == snapshot);
            PPR_TEST_ASSERT(loaded->m_seed == kSeed);
        };

        PPR_UNIT_TEST (save_load_round_trip_with_deltas_and_streams) {
            ChunkGrid grid{};
            PPR_TEST_ASSERT(not grid.setCell(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}));
            PPR_TEST_ASSERT(not grid.setCell(GlobalCellPos{200u, 5u}, Cell{7u, 300.0f}));

            constexpr u64 kSeed = 0xFACEB00Cull;
            Snapshot snapshot = capture(grid, kSeed);
            snapshot.m_streams.push_back(RngStream{1u, 0xDEADBEEFCAFEBABEull});
            snapshot.m_streams.push_back(RngStream{2u, 42u});

            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());

            // Header (version + seed + delta count) + two delta headers with
            // dense cell runs + stream count + two streams.
            PPR_TEST_ASSERT(saved.value().size()
                            == 16u + 2u * (8u + kCellsPerChunk * 6u) + 4u + 2u * 12u + 32u);

            const Expected<Snapshot> loaded = load(std::span<const u8>{saved.value()});
            PPR_TEST_ASSERT(loaded.has_value());
            PPR_TEST_ASSERT(*loaded == snapshot);
            PPR_TEST_ASSERT(loaded->m_deltas.size() == 2u);
            PPR_TEST_ASSERT(loaded->m_streams == snapshot.m_streams);
        };

        PPR_UNIT_TEST (byte_layout_is_stable) {
            const Snapshot snapshot = singleCellSnapshot(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}, 0x0123456789ABCDEFull);
            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());

            const std::span<const u8> bytes{saved.value()};

            // u32 version, u64 seed, u32 delta count, then the delta header.
            PPR_TEST_ASSERT(bytes.size() == 16u + 8u + kCellsPerChunk * 6u + 4u + 32u);
            PPR_TEST_ASSERT(readU32(bytes, 0u) == kSnapshotVersion);
            PPR_TEST_ASSERT(readU32(bytes, 12u) == 1u);
            PPR_TEST_ASSERT(readU32(bytes, 16u) == chunkIndexOf(ChunkPos{}));
            PPR_TEST_ASSERT(readU32(bytes, 20u) == kCellsPerChunk);
            const std::size_t ecs_offset = 16u + 8u + kCellsPerChunk * 6u + 4u;
            PPR_TEST_ASSERT(readU32(bytes, ecs_offset) == 1u);
            PPR_TEST_ASSERT(readU32(bytes, ecs_offset + 4u) == 0u);
            PPR_TEST_ASSERT(readU32(bytes, ecs_offset + 8u) == 0u);
            PPR_TEST_ASSERT(readU32(bytes, ecs_offset + 12u) == 0u);

            // Seed 0x0123456789ABCDEF stored little-endian at [4, 12).
            PPR_TEST_ASSERT(bytes[4] == 0xEFu);
            PPR_TEST_ASSERT(bytes[5] == 0xCDu);
            PPR_TEST_ASSERT(bytes[6] == 0xABu);
            PPR_TEST_ASSERT(bytes[7] == 0x89u);
            PPR_TEST_ASSERT(bytes[8] == 0x67u);
            PPR_TEST_ASSERT(bytes[9] == 0x45u);
            PPR_TEST_ASSERT(bytes[10] == 0x23u);
            PPR_TEST_ASSERT(bytes[11] == 0x01u);
        };

        PPR_UNIT_TEST (load_rejects_version_mismatch) {
            const Snapshot snapshot = singleCellSnapshot(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}, 1u);
            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());

            Array<u8> patched = saved.value();
            patched[0] = static_cast<u8>(kSnapshotVersion + 1u);

            const Expected<Snapshot> loaded = load(std::span<const u8>{patched});
            PPR_TEST_ASSERT(not loaded.has_value());
            PPR_TEST_ASSERT(loaded.error() == make_error_code(errc::version_mismatch));
        };

        PPR_UNIT_TEST (save_and_apply_reject_version_mismatch) {
            Snapshot stale{};
            stale.m_version = kSnapshotVersion + 1u;

            const Expected<Array<u8> > saved = save(stale);
            PPR_TEST_ASSERT(not saved.has_value());
            PPR_TEST_ASSERT(saved.error() == make_error_code(errc::version_mismatch));

            ChunkGrid grid{};
            PPR_TEST_ASSERT(apply(grid, stale) == make_error_code(errc::version_mismatch));
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
        };

        PPR_UNIT_TEST (load_rejects_truncated_input) {
            const Snapshot snapshot = singleCellSnapshot(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}, 1u);
            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());

            const std::span<const u8> bytes{saved.value()};

            // Fewer than the four version bytes.
            const Expected<Snapshot> headerless = load(bytes.first(2u));
            PPR_TEST_ASSERT(not headerless.has_value());
            PPR_TEST_ASSERT(headerless.error() == make_error_code(errc::truncated));

            // Version readable, seed cut short.
            const Expected<Snapshot> short_seed = load(bytes.first(10u));
            PPR_TEST_ASSERT(not short_seed.has_value());
            PPR_TEST_ASSERT(short_seed.error() == make_error_code(errc::truncated));

            // Delta payload cut short.
            const Expected<Snapshot> short_cells = load(bytes.first(bytes.size() - 1u));
            PPR_TEST_ASSERT(not short_cells.has_value());
            PPR_TEST_ASSERT(short_cells.error() == make_error_code(errc::truncated));
        };

        PPR_UNIT_TEST (load_rejects_malformed_payload) {
            const Snapshot snapshot = singleCellSnapshot(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}, 1u);
            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());
            const std::span<const u8> bytes{saved.value()};

            // Absurd delta count must be rejected before it reserves.
            Array<u8> huge_count = saved.value();
            huge_count[12] = 0xFFu;
            huge_count[13] = 0xFFu;
            huge_count[14] = 0xFFu;
            huge_count[15] = 0xFFu;
            const Expected<Snapshot> too_many = load(std::span<const u8>{huge_count});
            PPR_TEST_ASSERT(not too_many.has_value());
            PPR_TEST_ASSERT(too_many.error() == make_error_code(errc::invalid_payload));

            // Chunk index beyond the grid.
            Array<u8> bad_chunk = saved.value();
            bad_chunk[16] = 0x00u;
            bad_chunk[17] = 0x04u;
            bad_chunk[18] = 0x00u;
            bad_chunk[19] = 0x00u;
            const Expected<Snapshot> out_of_range = load(std::span<const u8>{bad_chunk});
            PPR_TEST_ASSERT(not out_of_range.has_value());
            PPR_TEST_ASSERT(out_of_range.error() == make_error_code(errc::invalid_payload));

            // Dense runs only: a cell count other than kCellsPerChunk is malformed.
            Array<u8> bad_count = saved.value();
            bad_count[20] = 0x07u;
            const Expected<Snapshot> short_run = load(std::span<const u8>{bad_count});
            PPR_TEST_ASSERT(not short_run.has_value());
            PPR_TEST_ASSERT(short_run.error() == make_error_code(errc::invalid_payload));

            // Trailing bytes are never silently dropped.
            Array<u8> trailing = saved.value();
            trailing.push_back(0u);
            const Expected<Snapshot> extra = load(std::span<const u8>{trailing});
            PPR_TEST_ASSERT(not extra.has_value());
            PPR_TEST_ASSERT(extra.error() == make_error_code(errc::invalid_payload));

            // Temperature bits of the first cell sit at 16 (header) + 8 (delta
            // header) + 2 (element) = 26; patched NaN fails the terminal gate.
            Array<u8> nan_bits = saved.value();
            nan_bits[26] = 0x00u;
            nan_bits[27] = 0x00u;
            nan_bits[28] = 0xC0u;
            nan_bits[29] = 0x7Fu;
            const Expected<Snapshot> nan_temperature = load(std::span<const u8>{nan_bits});
            PPR_TEST_ASSERT(not nan_temperature.has_value());
            PPR_TEST_ASSERT(nan_temperature.error() == make_error_code(errc::invalid_payload));

            // Positive infinity (0x7F800000) is rejected like NaN.
            Array<u8> inf_bits = saved.value();
            inf_bits[26] = 0x00u;
            inf_bits[27] = 0x00u;
            inf_bits[28] = 0x80u;
            inf_bits[29] = 0x7Fu;
            const Expected<Snapshot> inf_temperature = load(std::span<const u8>{inf_bits});
            PPR_TEST_ASSERT(not inf_temperature.has_value());
            PPR_TEST_ASSERT(inf_temperature.error() == make_error_code(errc::invalid_payload));

            Array<u8> bad_ecs_version = saved.value();
            const std::size_t ecs_offset = 16u + 8u + kCellsPerChunk * 6u + 4u;
            bad_ecs_version[ecs_offset] = 2u;
            const Expected<Snapshot> incompatible_ecs = load(std::span<const u8>{bad_ecs_version});
            PPR_TEST_ASSERT(not incompatible_ecs.has_value());
            PPR_TEST_ASSERT(incompatible_ecs.error() == make_error_code(errc::invalid_payload));
        };

        PPR_UNIT_TEST (duplicate_chunk_deltas_are_rejected) {
            ChunkGrid grid{};
            PPR_TEST_ASSERT(not grid.setCell(GlobalCellPos{10u, 20u}, Cell{3u, 12.5f}));

            const Snapshot snapshot = capture(grid, 1u);
            PPR_TEST_ASSERT(snapshot.m_deltas.size() == 1u);

            Snapshot duplicated = snapshot;
            duplicated.m_deltas.push_back(snapshot.m_deltas[0]);

            // Fail closed over last-wins: the repeated index never reaches the
            // grid, so no ordering of the duplicates could ever be observed.
            const Expected<Array<u8> > saved = save(duplicated);
            PPR_TEST_ASSERT(not saved.has_value());
            PPR_TEST_ASSERT(saved.error() == make_error_code(errc::invalid_payload));

            ChunkGrid target{};
            PPR_TEST_ASSERT(apply(target, duplicated) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(target.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(target.residentChunkCount() == 0u);
        };

        PPR_UNIT_TEST (apply_restores_the_grid) {
            const GlobalCellPos pos{10u, 20u};
            const Snapshot snapshot = singleCellSnapshot(pos, Cell{3u, 12.5f}, 1u);

            ChunkGrid target{};
            PPR_TEST_ASSERT(not apply(target, snapshot));

            const Expected<Cell> restored = target.getCell(pos);
            PPR_TEST_ASSERT(restored.has_value());
            PPR_TEST_ASSERT(*restored == (Cell{3u, 12.5f}));

            // Re-capturing the restored grid reproduces the snapshot.
            PPR_TEST_ASSERT(capture(target, snapshot.m_seed) == snapshot);
        };

        PPR_UNIT_TEST (apply_rejects_malformed_snapshots_without_mutation) {
            ChunkGrid grid{};

            Snapshot short_delta{};
            short_delta.m_deltas.push_back(ChunkDelta{0u, Array<Cell>{}});
            PPR_TEST_ASSERT(apply(grid, short_delta) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);

            Snapshot nan_delta{};
            nan_delta.m_deltas.push_back(ChunkDelta{0u, Array<Cell>(kCellsPerChunk, Cell{})});
            nan_delta.m_deltas[0].m_cells[0].m_temperature = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(apply(grid, nan_delta) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);

            Snapshot inf_delta{};
            inf_delta.m_deltas.push_back(ChunkDelta{0u, Array<Cell>(kCellsPerChunk, Cell{})});
            inf_delta.m_deltas[0].m_cells[0].m_temperature = std::numeric_limits<float>::infinity();
            PPR_TEST_ASSERT(apply(grid, inf_delta) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(grid.dirtyChunkCount() == 0u);
            PPR_TEST_ASSERT(grid.residentChunkCount() == 0u);
        };

        PPR_UNIT_TEST (apply_merges_into_a_pre_dirty_target) {
            const GlobalCellPos applied{10u, 20u};
            const GlobalCellPos pre_dirty{200u, 5u};

            ChunkGrid target{};
            PPR_TEST_ASSERT(not target.setCell(pre_dirty, Cell{21u, 320.0f}));
            PPR_TEST_ASSERT(target.dirtyChunkCount() == 1u);

            const Snapshot snapshot = singleCellSnapshot(applied, Cell{3u, 12.5f}, 1u);
            PPR_TEST_ASSERT(not apply(target, snapshot));

            // The unrelated chunk keeps both content and dirty state: apply
            // merges, it does not reset the target.
            const Expected<Cell> kept = target.getCell(pre_dirty);
            PPR_TEST_ASSERT(kept.has_value());
            PPR_TEST_ASSERT(*kept == (Cell{21u, 320.0f}));

            const Expected<Cell> restored = target.getCell(applied);
            PPR_TEST_ASSERT(restored.has_value());
            PPR_TEST_ASSERT(*restored == (Cell{3u, 12.5f}));

            PPR_TEST_ASSERT(target.dirtyChunkCount() == 2u);

            // The pre-dirty chunk survives as an extra delta, so re-capture
            // equals the applied snapshot only for a clean target.
            PPR_TEST_ASSERT(capture(target, snapshot.m_seed) != snapshot);
            PPR_TEST_ASSERT(capture(target, snapshot.m_seed).m_deltas.size() == 2u);
        };

        PPR_UNIT_TEST (rng_streams_survive_save_load) {
            Snapshot snapshot{};
            snapshot.m_seed = 7u;
            snapshot.m_streams.push_back(RngStream{1u, 0xDEADBEEFCAFEBABEull});
            snapshot.m_streams.push_back(RngStream{2u, 1u});

            const Expected<Array<u8> > saved = save(snapshot);
            PPR_TEST_ASSERT(saved.has_value());

            const Expected<Snapshot> loaded = load(std::span<const u8>{saved.value()});
            PPR_TEST_ASSERT(loaded.has_value());
            PPR_TEST_ASSERT(loaded->m_streams == snapshot.m_streams);
            PPR_TEST_ASSERT(loaded->m_streams[0] == (RngStream{1u, 0xDEADBEEFCAFEBABEull}));
            PPR_TEST_ASSERT(loaded->m_seed == 7u);
        };

        struct Position {
            i32 m_x{};
            i32 m_y{};
        };

        struct Velocity {
            u32 m_speed{};
        };

        PPR_UNIT_TEST (ecs_dump_save_load_restore_round_trip) {
            ChunkGrid grid{};
            Registry source{};
            PPR_TEST_ASSERT(source.registerComponent<Position>().has_value());
            PPR_TEST_ASSERT(source.registerComponent<Velocity>().has_value());
            const Entity first = source.create();
            const Entity dead = source.create();
            const Entity last = source.create();
            const Entity dead_last = source.create();
            const Entity dead_first = source.create();
            source.emplace(last, Position{30, 40});
            source.emplace(first, Position{10, 20});
            source.emplace(last, Velocity{7u});
            source.destroy(dead_first);
            source.destroy(dead);
            source.destroy(dead_last);

            const Snapshot snapshot = capture(grid, source, 71u);
            const Expected<Array<u8> > encoded = save(snapshot);
            PPR_TEST_ASSERT(encoded.has_value());
            const Expected<Snapshot> loaded = load(std::span<const u8>{*encoded});
            PPR_TEST_ASSERT(loaded.has_value());
            PPR_TEST_ASSERT(*loaded == snapshot);

            Registry target{};
            PPR_TEST_ASSERT(target.registerComponent<Position>().has_value());
            PPR_TEST_ASSERT(target.registerComponent<Velocity>().has_value());
            PPR_TEST_ASSERT(not restore(target, *loaded));
            PPR_TEST_ASSERT(target.dump() == source.dump());
            PPR_TEST_ASSERT(target.isAlive(first) and target.isAlive(last));
            PPR_TEST_ASSERT(not target.isAlive(dead));
            PPR_TEST_ASSERT(target.get<Position>(first)->m_x == 10);
            PPR_TEST_ASSERT(target.get<Velocity>(last)->m_speed == 7u);

            PPR_TEST_ASSERT(snapshot.m_ecs.m_free_order == (Array<u32>{dead_last.m_index, dead.m_index, dead_first.m_index}));
            for (u32 allocation = 0u; allocation < 2u; ++allocation) {
                const Entity source_created = source.create();
                const Entity target_created = target.create();
                PPR_TEST_ASSERT(source_created == target_created);
            }

            StepRegistry scheduler{};
            const Expected<u32> spawn = scheduler.declare("spawn");
            PPR_TEST_ASSERT(spawn.has_value());
            Array<Entity> spawned{};
            PPR_TEST_ASSERT(scheduler.addSystem(*spawn, [&](Registry &registry) {
                const Entity entity = registry.create();
                registry.emplace(entity, Position{42, 17});
                spawned.push_back(entity);
            }).has_value());
            PPR_TEST_ASSERT(not scheduler.runSystems(source));
            PPR_TEST_ASSERT(not scheduler.runSystems(target));
            PPR_TEST_ASSERT(spawned.size() == 2u);
            PPR_TEST_ASSERT(spawned[0] == spawned[1]);
            PPR_TEST_ASSERT(target.dump() == source.dump());
        };

        PPR_UNIT_TEST (ecs_restore_replaces_and_rejects_bad_layout_without_mutation) {
            ChunkGrid grid{};
            Registry target{};
            PPR_TEST_ASSERT(target.registerComponent<Position>().has_value());
            const Entity old = target.create();
            target.emplace(old, Position{9, 8});

            Registry source{};
            PPR_TEST_ASSERT(source.registerComponent<Position>().has_value());
            const Entity first = source.create();
            source.destroy(first);
            const Entity replacement = source.create();
            source.emplace(replacement, Position{2, 3});
            const Entity dead_a = source.create();
            const Entity dead_b = source.create();
            source.destroy(dead_a);
            source.destroy(dead_b);
            const Snapshot snapshot = capture(grid, source, 12u);

            Snapshot bad = snapshot;
            bad.m_ecs.m_columns[0].m_stride += 1u;
            PPR_TEST_ASSERT(restore(target, bad) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(target.get<Position>(old)->m_x == 9);
            bad = snapshot;
            bad.m_ecs.m_columns[0].m_owners[0].m_generation += 1u;
            PPR_TEST_ASSERT(restore(target, bad) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(target.get<Position>(old)->m_x == 9);

            bad = snapshot;
            bad.m_ecs.m_free_order[0] = replacement.m_index;
            const EcsSection unchanged = target.dump();
            PPR_TEST_ASSERT(restore(target, bad) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(target.dump() == unchanged);
            PPR_TEST_ASSERT(not save(bad).has_value());

            bad.m_ecs.m_free_order = snapshot.m_ecs.m_free_order;
            bad.m_ecs.m_free_order[0] = bad.m_ecs.m_free_order[1];
            PPR_TEST_ASSERT(restore(target, bad) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(target.dump() == unchanged);
            bad.m_ecs.m_free_order = snapshot.m_ecs.m_free_order;
            bad.m_ecs.m_free_order[0] = source.slotCount();
            PPR_TEST_ASSERT(restore(target, bad) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(target.dump() == unchanged);
            bad.m_ecs.m_free_order.pop_back();
            PPR_TEST_ASSERT(restore(target, bad) == make_error_code(errc::invalid_payload));
            PPR_TEST_ASSERT(target.dump() == unchanged);

            PPR_TEST_ASSERT(not restore(target, snapshot));
            PPR_TEST_ASSERT(not target.isAlive(old));
            PPR_TEST_ASSERT(target.isAlive(replacement));
            PPR_TEST_ASSERT(target.get<Position>(replacement)->m_x == 2);
            PPR_TEST_ASSERT(target.dump() == source.dump());
        };

        PPR_UNIT_TEST (v1_snapshot_is_rejected_before_ecs_mutation) {
            ChunkGrid grid{};
            Registry registry{};
            PPR_TEST_ASSERT(registry.registerComponent<Position>().has_value());
            const Entity entity = registry.create();
            registry.emplace(entity, Position{5, 6});
            const Snapshot snapshot = capture(grid, registry, 1u);
            const Expected<Array<u8> > encoded = save(snapshot);
            PPR_TEST_ASSERT(encoded.has_value());

            // A v1 file has no ECS tail; the version gate must reject it first.
            Array<u8> old_bytes(encoded->begin(), encoded->begin() + 20u);
            old_bytes[0] = 1u;
            const Expected<Snapshot> old = load(std::span<const u8>{old_bytes});
            PPR_TEST_ASSERT(not old.has_value());
            PPR_TEST_ASSERT(old.error() == make_error_code(errc::version_mismatch));

            Snapshot stale = snapshot;
            stale.m_version = 1u;
            PPR_TEST_ASSERT(restore(registry, stale) == make_error_code(errc::version_mismatch));
            PPR_TEST_ASSERT(registry.dump() == snapshot.m_ecs);
        };
    } // namespace SnapshotSuite
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest snapshot = UnitTest::Named("snapshot") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::SnapshotSuite::capture_reads_dirty_chunks,
            detail::SnapshotSuite::capture_orders_two_dirty_chunks_row_major,
            detail::SnapshotSuite::capture_of_marked_dirty_chunk_is_zero_filled,
            detail::SnapshotSuite::capture_of_clean_grid_is_empty,
            detail::SnapshotSuite::save_load_round_trip,
            detail::SnapshotSuite::save_load_round_trip_with_deltas_and_streams,
            detail::SnapshotSuite::byte_layout_is_stable,
            detail::SnapshotSuite::load_rejects_version_mismatch,
            detail::SnapshotSuite::save_and_apply_reject_version_mismatch,
            detail::SnapshotSuite::load_rejects_truncated_input,
            detail::SnapshotSuite::load_rejects_malformed_payload,
            detail::SnapshotSuite::duplicate_chunk_deltas_are_rejected,
            detail::SnapshotSuite::apply_restores_the_grid,
            detail::SnapshotSuite::apply_merges_into_a_pre_dirty_target,
            detail::SnapshotSuite::apply_rejects_malformed_snapshots_without_mutation,
            detail::SnapshotSuite::rng_streams_survive_save_load,
            detail::SnapshotSuite::ecs_dump_save_load_restore_round_trip,
            detail::SnapshotSuite::ecs_restore_replaces_and_rejects_bad_layout_without_mutation,
            detail::SnapshotSuite::v1_snapshot_is_rejected_before_ecs_mutation,
        });
    };

    const UnitTest &snapshotTests() noexcept {
        return snapshot;
    }
} // namespace pP::tests
