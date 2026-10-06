module;

#include "Colony.Elements.h"

module game.colony.sim_packet;

import std;

namespace pP::colony {
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
        packet.m_tick = driver.tickCount();
        packet.m_dig_total = driver.digTotal();
        packet.m_agent_count = static_cast<u32>(driver.agentSummaries().size());
        packet.m_path_counts = driver.pathCounts();
        packet.m_errands = driver.errandCount();
        packet.m_full_refresh = full_refresh;
        for (const sim::ChunkPos chunk: driver.presentDirtyChunks()) {
            packet.m_present_chunks.push_back(chunk);
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

    bool isSimPacketCoherent(const SimFramePacket &packet, const u64 live_tick, const u64 live_seed) noexcept {
        return packet.m_tick == live_tick and packet.m_seed == live_seed;
    }

    std::error_code presentSimPacket(
        const SimFramePacket &packet, ColonyDriver &driver, ColonyTranslator &translator, GridPass &pass) {
        // Selective acknowledgement at the defined consumption point: mark
        // and ack ONLY the captured present set. Dirt that arrived after
        // capture stays pending in the driver (unmarked here) for the next
        // frame, so the upload set below — read from packet cells only —
        // and the ack set always agree. A mark failure acks nothing, so the
        // captured dirt is retained for retry instead of being dropped.
        for (const sim::ChunkPos pos: packet.m_present_chunks) {
            if (const std::error_code error = translator.markChunkChanged(pos)) {
                return error;
            }
        }
        driver.ackPresentChunks(
            std::span<const sim::ChunkPos>{packet.m_present_chunks.data(), packet.m_present_chunks.size()});

        // Stage exactly the capture-time residency. Dirty marks come from
        // the packet (or the full-refresh flag), never from translator or
        // live state, so post-capture mutation cannot alter the frame.
        Array<GridTileSubmission> tiles{};
        tiles.reserve(packet.m_resident_chunks.size());
        for (const sim::ChunkPos pos: packet.m_resident_chunks) {
            const bool dirty = packet.m_full_refresh or containsChunk(packet.m_present_chunks, pos);
            tiles.push_back(
                GridTileSubmission{
                    .m_chunk_id = sim::chunkIndexOf(pos),
                    .m_tile_range = tileRange(pos),
                    .m_dirty_mask = dirty ? 1u : 0u,
                    .m_material_id = 0u,
                });
        }
        if (const std::error_code error = pass.submitTiles(
                std::span<const GridTileSubmission>{tiles.data(), tiles.size()})) [[unlikely]] {
            translator.reset(pass);
            return error;
        }

        // Upload captured contents for dirty chunks only; clean chunks keep
        // serving from the pass cache. A dirty chunk without captured cells
        // (or with a corrupt payload) fails closed before touching the pass.
        Array<u16> scratch{};
        for (const GridTileSubmission &tile: tiles) {
            if (tile.m_dirty_mask == 0u) {
                continue;
            }
            const sim::ChunkPos pos{tile.m_chunk_id % sim::kChunksPerEdge, tile.m_chunk_id / sim::kChunksPerEdge};
            const Array<sim::Cell> *const cells = findCells(packet.m_chunk_cells, pos);
            if (cells == nullptr or cells->size() != sim::kCellsPerChunk) [[unlikely]] {
                translator.reset(pass);
                return std::make_error_code(std::errc::invalid_argument);
            }
            if (const std::error_code error = convertCells(*cells, scratch)) [[unlikely]] {
                translator.reset(pass);
                return error;
            }
            GridTilePayload payload{};
            payload.m_rect[0] = static_cast<float>(tile.m_tile_range.m_min_x);
            payload.m_rect[1] = static_cast<float>(tile.m_tile_range.m_min_y);
            payload.m_rect[2] = static_cast<float>(tile.m_tile_range.m_max_x);
            payload.m_rect[3] = static_cast<float>(tile.m_tile_range.m_max_y);
            payload.m_material = 0u;
            const Expected<u32> uploaded = pass.requestUpload(
                GridUploadRequest{
                    .m_chunk_id = tile.m_chunk_id,
                    .m_tile_data_view = std::span<const GridTilePayload>{&payload, 1u},
                    .m_cell_materials = std::span<const u16>{scratch.data(), scratch.size()},
                });
            if (not uploaded) [[unlikely]] {
                translator.reset(pass);
                return uploaded.error();
            }
            if (*uploaded != 1u) [[unlikely]] {
                translator.reset(pass);
                return std::make_error_code(std::errc::state_not_recoverable);
            }
        }
        return {};
    }
}
