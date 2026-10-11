module;

#include "Colony.Elements.h"
#include "pP/Macros.h"

module game.colony.sim_packet;

import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(SimPresentation, info, none);
    namespace {
        [[nodiscard]] GridTileRange tileRange(const sim::ChunkPos pos) noexcept {
            const i32 min_x = static_cast<i32>(pos.m_x * sim::kChunkEdge);
            const i32 min_y = static_cast<i32>(pos.m_y * sim::kChunkEdge);
            return {min_x, min_y, min_x + static_cast<i32>(sim::kChunkEdge), min_y + static_cast<i32>(sim::kChunkEdge)};
        }

        [[nodiscard]] bool containsChunk(const Array<sim::ChunkPos> &chunks, const sim::ChunkPos pos) noexcept {
            for (const sim::ChunkPos chunk: chunks) {
                if (chunk == pos) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] const Array<sim::Cell> *findCells(
            const Array<SimChunkCells> &payloads, const sim::ChunkPos pos) noexcept {
            for (const SimChunkCells &payload: payloads) {
                if (payload.m_pos == pos) {
                    return &payload.m_cells;
                }
            }
            return nullptr;
        }

        // Mirrors ColonyTranslator's element mapping: vacuum is transparent,
        // anything above the registry is a fail-closed validation error.
        // Allocation failure propagates exactly as in the translator.
        [[nodiscard]] std::error_code convertCells(const Array<sim::Cell> &cells, Array<u16> &scratch) {
            scratch.resize(sim::kCellsPerChunk);
            for (std::size_t cell = 0u; cell < sim::kCellsPerChunk; ++cell) {
                const u16 element = cells[cell].m_element;
                if (element > kElementVent) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                scratch[cell] = element == kElementVacuum ? u16{0xffffu} : element;
            }
            return {};
        }
    }

    SimFramePacket makeSimFramePacket(const u64 frame_id, const ColonyDriver &driver, const bool full_refresh) {
        SimFramePacket packet{};
        packet.m_frame_id = frame_id;
        packet.m_seed = driver.seed();
        packet.m_world_generation = driver.worldGeneration();
        packet.m_tick = driver.tickCount();
        packet.m_dig_total = driver.digTotal();
        packet.m_agent_count = static_cast<u32>(driver.agentSummaries().size());
        packet.m_path_counts = driver.pathCounts();
        packet.m_errands = driver.errandCount();
        packet.m_full_refresh = full_refresh;
        for (const ColonyDriver::DirtyChunk &row: driver.presentEditsCapture()) {
            packet.m_present_chunks.push_back(row.m_pos);
            packet.m_present_revisions.push_back(row.m_revision);
        }
        const sim::ChunkGrid &grid = driver.grid();
        for (u32 y = 0u; y < sim::kChunksPerEdge; ++y) {
            for (u32 x = 0u; x < sim::kChunksPerEdge; ++x) {
                const sim::ChunkPos pos{x, y};
                if (not grid.isResident(pos)) {
                    continue;
                }
                packet.m_resident_chunks.push_back(pos);
                if (not full_refresh and not containsChunk(packet.m_present_chunks, pos)) {
                    continue;
                }
                const std::optional<std::span<const sim::Cell>> cells = grid.residentCells(pos);
                if (not cells or cells->size() != sim::kCellsPerChunk) [[unlikely]] {
                    continue;
                }
                SimChunkCells payload{};
                payload.m_pos = pos;
                payload.m_cells.assign(cells->begin(), cells->end());
                packet.m_chunk_cells.push_back(std::move(payload));
            }
        }
        return packet;
    }

    bool isSimPacketCoherent(
        const SimFramePacket &packet, const u64 live_tick, const u64 live_world_generation) noexcept {
        return packet.m_tick == live_tick and packet.m_world_generation == live_world_generation;
    }

    Expected<SimFramePacket> makeInitialPresentationBatch(
        const u64 frame_id, const ColonyDriver &driver, const u32 first_chunk, const u32 chunk_count) {
        if (chunk_count == 0u or
            chunk_count > kInitialPresentationBatchChunks or
            first_chunk >= sim::kChunkCount or
            chunk_count > sim::kChunkCount - first_chunk) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, std::errc::invalid_argument);
        }
        SimFramePacket packet{};
        packet.m_frame_id = frame_id;
        packet.m_seed = driver.seed();
        packet.m_world_generation = driver.worldGeneration();
        packet.m_tick = driver.tickCount();
        packet.m_agent_count = static_cast<u32>(driver.agentSummaries().size());
        const auto dirty = driver.presentEditsCapture();
        const auto &grid = driver.grid();
        const u32 end = first_chunk + chunk_count;
        for (u32 index = 0u; index < end; ++index) {
            const sim::ChunkPos pos{index % sim::kChunksPerEdge, index / sim::kChunksPerEdge};
            if (not grid.isResident(pos)) {
                continue;
            }
            packet.m_resident_chunks.push_back(pos);
            if (index < first_chunk) {
                continue;
            }
            const auto cells = grid.residentCells(pos);
            if (not cells or cells->size() != sim::kCellsPerChunk) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, std::errc::state_not_recoverable);
            }
            u64 revision{};
            for (const auto &row: dirty) {
                if (row.m_pos == pos) {
                    revision = row.m_revision;
                    break;
                }
            }
            packet.m_present_chunks.push_back(pos);
            packet.m_present_revisions.push_back(revision);
            SimChunkCells payload{.m_pos = pos};
            payload.m_cells.assign(cells->begin(), cells->end());
            packet.m_chunk_cells.push_back(std::move(payload));
        }
        return packet;
    }

    u32 InitialPresentation::completedChunks() const noexcept {
        return m_next_chunk;
    }

    Expected<GridPass::GridStagedFrame> InitialPresentation::stage(const u64 frame_id, const ColonyDriver &driver) {
        GridPass::GridStagedFrame stage{};
        if (m_frame or m_next_chunk == sim::kChunkCount) {
            stage.m_tiles = m_visible;
            return stage;
        }
        const u32 count = std::min(kInitialPresentationBatchChunks, sim::kChunkCount - m_next_chunk);
        auto packet = makeInitialPresentationBatch(frame_id, driver, m_next_chunk, count);
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, packet);
        PPR_RETURN_UNEXPECTED_ON_FAIL(SimPresentation, buildGridStagedFrame(*packet, stage));
        stage.m_reset_caches = m_next_chunk == 0u;
        m_pending = stage.m_tiles;
        for (auto &tile: m_pending) {
            tile.m_dirty_mask = 0u;
        }
        m_frame = frame_id;
        return stage;
    }

    bool InitialPresentation::consume(const RenderFrameResult &result, const u64 world_generation) {
        if (m_frame != result.m_frame_id or result.m_grid_ack_seed != world_generation) {
            return false;
        }
        m_frame.reset();
        if (not result.m_presented or not result.m_grid_stage_applied or result.m_error or result.m_dropped) {
            return false;
        }
        m_visible = std::move(m_pending);
        m_next_chunk = std::min(m_next_chunk + kInitialPresentationBatchChunks, sim::kChunkCount);
        return m_next_chunk == sim::kChunkCount;
    }

    std::error_code buildGridStagedFrame(const SimFramePacket &packet, GridPass::GridStagedFrame &stage) {
        // Owned payload only: no driver ack, no translator contact, and no
        // pass mutation happen on the client. The pass-side contact runs
        // on the pass's owner lane via GridPass::applyStagedFrame.
        // The consumption-ack manifest rides inside the owned stage: the
        // lane only round-trips it into the RenderFrameResult, and the
        // derived owner consumes it at settlement. Derived from the owned
        // packet only — never the live driver state.
        stage.m_full_refresh = packet.m_full_refresh;
        stage.m_ack_seed = packet.m_world_generation;
        if (packet.m_present_chunks.size() != packet.m_present_revisions.size()) [[unlikely]] {
            stage.m_ack_chunk_ids.clear();
            stage.m_ack_revisions.clear();
            return std::make_error_code(std::errc::invalid_argument);
        }
        stage.m_ack_chunk_ids.clear();
        stage.m_ack_chunk_ids.reserve(packet.m_present_chunks.size());
        stage.m_ack_revisions.clear();
        stage.m_ack_revisions.reserve(packet.m_present_revisions.size());
        for (u64 i = 0u; i < packet.m_present_chunks.size(); ++i) {
            stage.m_ack_chunk_ids.push_back(sim::chunkIndexOf(packet.m_present_chunks[i]));
            stage.m_ack_revisions.push_back(packet.m_present_revisions[i]);
        }

        // Stage exactly the capture-time residency. Dirty marks come from
        // the packet (or the full-refresh flag), never from translator or
        // live state, so post-capture mutation cannot alter the frame.
        stage.m_tiles.clear();
        stage.m_tiles.reserve(packet.m_resident_chunks.size());
        for (const sim::ChunkPos pos: packet.m_resident_chunks) {
            const bool dirty = packet.m_full_refresh or containsChunk(packet.m_present_chunks, pos);
            stage.m_tiles.push_back(
                GridTileSubmission{
                    .m_chunk_id = sim::chunkIndexOf(pos),
                    .m_tile_range = tileRange(pos),
                    .m_dirty_mask = dirty ? 1u : 0u,
                    .m_material_id = 0u,
                });
        }

        // Validate + convert the captured contents now so a corrupt payload
        // fails closed on the client (never as a half-applied pass). The
        // converted u16 words and the one-element tile payload travel as
        // owned copies, ready for applyStagedFrame's requestUpload call.
        stage.m_uploads.clear();
        stage.m_uploads.reserve(packet.m_present_chunks.size() + packet.m_chunk_cells.size());
        for (const GridTileSubmission &tile: stage.m_tiles) {
            if (tile.m_dirty_mask == 0u) {
                continue;
            }
            const sim::ChunkPos pos{tile.m_chunk_id % sim::kChunksPerEdge, tile.m_chunk_id / sim::kChunksPerEdge};
            const Array<sim::Cell> *const cells = findCells(packet.m_chunk_cells, pos);
            if (cells == nullptr or cells->size() != sim::kCellsPerChunk) [[unlikely]] {
                stage.m_tiles.clear();
                stage.m_uploads.clear();
                stage.m_full_refresh = false;
                stage.m_ack_seed = 0u;
                stage.m_ack_chunk_ids.clear();
                stage.m_ack_revisions.clear();
                return std::make_error_code(std::errc::invalid_argument);
            }
            Array<u16> converted{};
            if (const std::error_code error = convertCells(*cells, converted)) [[unlikely]] {
                stage.m_tiles.clear();
                stage.m_uploads.clear();
                stage.m_full_refresh = false;
                stage.m_ack_seed = 0u;
                stage.m_ack_chunk_ids.clear();
                stage.m_ack_revisions.clear();
                return error;
            }
            GridPass::GridStagedUpload upload{};
            upload.m_chunk_id = tile.m_chunk_id;
            upload.m_payload.m_rect[0] = static_cast<float>(tile.m_tile_range.m_min_x);
            upload.m_payload.m_rect[1] = static_cast<float>(tile.m_tile_range.m_min_y);
            upload.m_payload.m_rect[2] = static_cast<float>(tile.m_tile_range.m_max_x);
            upload.m_payload.m_rect[3] = static_cast<float>(tile.m_tile_range.m_max_y);
            upload.m_payload.m_material = 0u;
            upload.m_cell_materials = std::move(converted);
            stage.m_uploads.push_back(std::move(upload));
        }
        return {};
    }
}
