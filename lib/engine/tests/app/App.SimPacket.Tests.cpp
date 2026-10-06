module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.sim;
import std;

import game.colony.driver;
import game.colony.sim_packet;
import game.colony.translator;

namespace pP::tests::detail {
    namespace SimPacket {
        // F1 game payload: owned copies captured after the tick, consumed by
        // the present/overlay phases on the same thread. These tests run the
        // production capture (makeSimFramePacket) and consume
        // (presentSimPacket) path, the fail-closed coherence gate, and the
        // translator input error paths — mostly without a live world
        // (default-constructed driver/translator); the capture/consume
        // tests initialize paused worlds for real present sets.

        PPR_UNIT_TEST (default_driver_builds_empty_coherent_packet) {
            colony::ColonyDriver driver{};
            colony::SimFramePacket packet = colony::makeSimFramePacket(3u, driver);

            PPR_TEST_ASSERT(packet.m_frame_id == 3u);
            PPR_TEST_ASSERT(packet.m_tick == driver.tickCount());
            PPR_TEST_ASSERT(packet.m_seed == driver.seed());
            PPR_TEST_ASSERT(packet.m_dig_total == driver.digTotal());
            PPR_TEST_ASSERT(packet.m_present_chunks.empty());
            PPR_TEST_ASSERT(packet.m_resident_chunks.empty());
            PPR_TEST_ASSERT(packet.m_chunk_cells.empty());
            PPR_TEST_ASSERT(not packet.m_full_refresh);
            PPR_TEST_ASSERT(colony::isSimPacketCoherent(packet, driver.tickCount(), driver.seed()));
        };

        // F1 game payload, real capture→mutation→consumer path: the packet
        // carries the chunk-content render payload, and same-thread present
        // preparation consumes the packet — never the live grid. Poisoning
        // the live cells with an invalid element after capture cannot alter
        // consumption (a live read would fail closed); only the captured
        // content reaches the pass.
        PPR_UNIT_TEST (capture_mutation_consume_uses_packet_content) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.init(colony::ColonyDriverDesc{.m_seed = 1234567u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };

            // One rock cell dug through the paused tool path, so the present
            // set and the dig counters populate exactly as in production.
            // (Element 1 is rock per the colony element registry; the dig
            // clears it to vacuum, element 0.)
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));
            PPR_TEST_ASSERT(driver.lastEditChunks().size() == 1u);
            PPR_TEST_ASSERT(driver.lastEditChunks()[0u] == sim::ChunkPos{0u, 0u});
            PPR_TEST_ASSERT(driver.digTotal() == 1u);

            // Real capture: residency, the present chunk, and its cells.
            colony::SimFramePacket packet = colony::makeSimFramePacket(9u, driver);
            PPR_TEST_ASSERT(packet.m_present_chunks.size() == 1u);
            PPR_TEST_ASSERT(packet.m_chunk_cells.size() == 1u);
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_pos == sim::ChunkPos{0u, 0u});
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_cells.size() == sim::kCellsPerChunk);
            constexpr u32 kDugCell = 10u * sim::kChunkEdge + 10u;
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_cells[kDugCell].m_element == 0u);

            // Post-capture producer mutation: overwrite the live chunk with
            // invalid elements (above the element registry). The owned packet
            // keeps the captured vacuum cell.
            Array<sim::Cell> poison{};
            poison.assign(sim::kCellsPerChunk, sim::Cell{.m_element = 999u, .m_temperature = 0.0f});
            PPR_TEST_ASSERT(not driver.grid().setCells(sim::ChunkPos{0u, 0u}, std::move(poison)));
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_cells[kDugCell].m_element == 0u);

            // Real consumer: succeeds (a live-grid read would fail closed on
            // the poisoned cells), stages exactly the capture-time residency,
            // and uploads only the packet-dirty chunk. Tick/seed never moved,
            // so the packet stays coherent across the mutation.
            colony::ColonyTranslator translator{};
            GridPass pass{};
            PPR_TEST_ASSERT(not colony::presentSimPacket(packet, driver, translator, pass));
            PPR_TEST_ASSERT(pass.stagedTileCount() == driver.grid().residentChunkCount());
            PPR_TEST_ASSERT(pass.stagedTileCount() == packet.m_resident_chunks.size());
            PPR_TEST_ASSERT(pass.uploadCount() == 1u);
            PPR_TEST_ASSERT(colony::isSimPacketCoherent(packet, driver.tickCount(), driver.seed()));
        };

        // Gate 0 multi-edit accumulation: two digs applied by two paused
        // updates (no present between them) leave BOTH chunks in the pending
        // present-dirty set while lastEditChunks reports only the second.
        // One capture snapshots the complete set and one present uploads
        // both, acking everything — a follow-up frame with no new edits
        // uploads nothing, so no stale remainder survives a successful
        // present (which clears the full-refresh flag in production).
        PPR_UNIT_TEST (two_chunks_across_updates_capture_and_upload_both) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.init(colony::ColonyDriverDesc{.m_seed = 1234567u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };

            // Rock in two distinct chunks (chunk edge 128): (0, 0) and (1, 0).
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            PPR_TEST_ASSERT(
                not driver.grid().setCell({200u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));
            PPR_TEST_ASSERT(not driver.requestDig({200u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            // Both edits still pending; the last-edit view reports only the second.
            PPR_TEST_ASSERT(driver.presentDirtyChunks().size() == 2u);
            PPR_TEST_ASSERT(driver.lastEditChunks().size() == 1u);
            PPR_TEST_ASSERT(driver.lastEditChunks()[0u] == sim::ChunkPos{1u, 0u});

            colony::SimFramePacket packet = colony::makeSimFramePacket(11u, driver);
            PPR_TEST_ASSERT(packet.m_present_chunks.size() == 2u);
            PPR_TEST_ASSERT(packet.m_present_chunks[0u] == sim::ChunkPos{0u, 0u});
            PPR_TEST_ASSERT(packet.m_present_chunks[1u] == sim::ChunkPos{1u, 0u});
            PPR_TEST_ASSERT(packet.m_chunk_cells.size() == 2u);

            colony::ColonyTranslator translator{};
            GridPass pass{};
            PPR_TEST_ASSERT(not colony::presentSimPacket(packet, driver, translator, pass));
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
            PPR_TEST_ASSERT(driver.presentDirtyChunks().empty());

            // Next frame with no new edits uploads nothing: nothing went stale.
            colony::SimFramePacket follow = colony::makeSimFramePacket(12u, driver);
            PPR_TEST_ASSERT(follow.m_present_chunks.empty());
            PPR_TEST_ASSERT(follow.m_chunk_cells.empty());
            colony::ColonyTranslator follow_translator{};
            GridPass follow_pass{};
            PPR_TEST_ASSERT(not colony::presentSimPacket(follow, driver, follow_translator, follow_pass));
            PPR_TEST_ASSERT(follow_pass.uploadCount() == 0u);
            PPR_TEST_ASSERT(follow_pass.stagedTileCount() == follow.m_resident_chunks.size());
        };

        // Full-refresh capture covers every resident chunk's cells (the
        // regeneration path that empties the pass cache), on a default
        // driver with hand-seeded chunks — no world init needed.
        PPR_UNIT_TEST (full_refresh_captures_all_resident_cells) {
            colony::ColonyDriver driver{};
            Array<sim::Cell> rock{};
            rock.assign(sim::kCellsPerChunk, sim::Cell{.m_element = 1u, .m_temperature = 293.0f});
            Array<sim::Cell> rock_copy = rock;
            PPR_TEST_ASSERT(not driver.grid().setCells(sim::ChunkPos{0u, 0u}, std::move(rock)));
            PPR_TEST_ASSERT(not driver.grid().setCells(sim::ChunkPos{1u, 2u}, std::move(rock_copy)));

            colony::SimFramePacket packet = colony::makeSimFramePacket(4u, driver, true);
            PPR_TEST_ASSERT(packet.m_full_refresh);
            PPR_TEST_ASSERT(packet.m_present_chunks.empty());
            PPR_TEST_ASSERT(packet.m_resident_chunks.size() == 2u);
            PPR_TEST_ASSERT(packet.m_chunk_cells.size() == 2u);

            colony::ColonyTranslator translator{};
            GridPass pass{};
            PPR_TEST_ASSERT(not colony::presentSimPacket(packet, driver, translator, pass));
            PPR_TEST_ASSERT(pass.stagedTileCount() == 2u);
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
        };

        // Fail-closed consumption: a dirty chunk without captured cells (or
        // with a corrupt payload) resets the translator/pass pair and
        // reports invalid_argument — never a partial present.
        PPR_UNIT_TEST (consume_rejects_dirty_chunk_without_cells) {
            colony::ColonyDriver driver{};
            colony::SimFramePacket packet{};
            packet.m_resident_chunks.push_back(sim::ChunkPos{0u, 0u});
            packet.m_present_chunks.push_back(sim::ChunkPos{0u, 0u});

            colony::ColonyTranslator translator{};
            GridPass pass{};
            PPR_TEST_ASSERT(
                colony::presentSimPacket(packet, driver, translator, pass) ==
                std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(pass.stagedTileCount() == 0u);

            Array<sim::Cell> corrupt{};
            corrupt.assign(sim::kCellsPerChunk, sim::Cell{.m_element = 999u, .m_temperature = 0.0f});
            colony::SimChunkCells payload{};
            payload.m_pos = sim::ChunkPos{0u, 0u};
            payload.m_cells = std::move(corrupt);
            packet.m_chunk_cells.push_back(std::move(payload));

            colony::ColonyTranslator retry_translator{};
            GridPass retry_pass{};
            PPR_TEST_ASSERT(
                colony::presentSimPacket(packet, driver, retry_translator, retry_pass) ==
                std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(retry_pass.stagedTileCount() == 0u);
        };

        PPR_UNIT_TEST (coherence_rejects_stale_tick_and_regenerated_world) {
            colony::SimFramePacket packet{};
            packet.m_tick = 5u;
            packet.m_seed = 11u;

            PPR_TEST_ASSERT(colony::isSimPacketCoherent(packet, 5u, 11u));
            PPR_TEST_ASSERT(not colony::isSimPacketCoherent(packet, 6u, 11u));
            PPR_TEST_ASSERT(not colony::isSimPacketCoherent(packet, 5u, 12u));
        };

        PPR_UNIT_TEST (mark_chunk_validates_positions) {
            colony::ColonyTranslator translator{};
            PPR_TEST_ASSERT(not translator.markChunkChanged(sim::ChunkPos{0u, 0u}));
            PPR_TEST_ASSERT(
                translator.markChunkChanged(sim::ChunkPos{sim::kChunksPerEdge, 0u}) ==
                std::make_error_code(std::errc::invalid_argument));
        };

        PPR_UNIT_TEST (empty_present_drain_succeeds) {
            // Draining a default driver marks nothing and reports success:
            // the present path is well-defined with zero edits.
            colony::ColonyDriver driver{};
            colony::ColonyTranslator translator{};
            PPR_TEST_ASSERT(not driver.presentEdits(translator));
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest sim_packet = UnitTest::Named("sim_packet") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::SimPacket::default_driver_builds_empty_coherent_packet,
            detail::SimPacket::capture_mutation_consume_uses_packet_content,
            detail::SimPacket::two_chunks_across_updates_capture_and_upload_both,
            detail::SimPacket::full_refresh_captures_all_resident_cells,
            detail::SimPacket::consume_rejects_dirty_chunk_without_cells,
            detail::SimPacket::coherence_rejects_stale_tick_and_regenerated_world,
            detail::SimPacket::mark_chunk_validates_positions,
            detail::SimPacket::empty_present_drain_succeeds,
        });
    };

    const UnitTest &sim_packetTests() noexcept {
        return sim_packet;
    }
} // namespace pP::tests
