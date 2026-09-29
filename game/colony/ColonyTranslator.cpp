module;
#include "Colony.Elements.h"
#include "StageTiming.h"

module game.colony.translator;

import engine.app;
import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    namespace {
        [[nodiscard]] GridTileRange tileRange(const sim::ChunkPos pos) noexcept {
            const i32 min_x = static_cast<i32>(pos.m_x * sim::kChunkEdge);
            const i32 min_y = static_cast<i32>(pos.m_y * sim::kChunkEdge);
            return {min_x, min_y, min_x + static_cast<i32>(sim::kChunkEdge), min_y + static_cast<i32>(sim::kChunkEdge)};
        }

        [[nodiscard]] std::error_code uploadChunk(const sim::ChunkGrid &grid, GridPass &pass,
                                                  const sim::ChunkPos pos, Array<u16> &scratch) {
            const auto cells = grid.residentCells(pos);
            if (not cells or cells->size() != sim::kCellsPerChunk)
            [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            scratch.resize(sim::kCellsPerChunk);
            for (std::size_t cell = 0u; cell < cells->size(); ++cell) {
                const u16 element = (*cells)[cell].m_element;
                if (element > kElementVent) [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                scratch[cell] = element == kElementVacuum ? u16{0xffffu} : element;
            }

            const GridTileRange range = tileRange(pos);
            GridTilePayload payload{};
            payload.m_rect[0] = static_cast<float>(range.m_min_x);
            payload.m_rect[1] = static_cast<float>(range.m_min_y);
            payload.m_rect[2] = static_cast<float>(range.m_max_x);
            payload.m_rect[3] = static_cast<float>(range.m_max_y);
            payload.m_material = 0u;

            const auto uploaded = pass.requestUpload(GridUploadRequest{
                .m_chunk_id = sim::chunkIndexOf(pos),
                .m_tile_data_view = std::span<const GridTilePayload>{&payload, 1u},
                .m_cell_materials = std::span<const u16>{scratch.data(), scratch.size()},
            });
            if (not uploaded) [[unlikely]] {
                return uploaded.error();
            }
            if (*uploaded != 1u) [[unlikely]] {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
            return {};
        }
    }

    void ColonyTranslator::reset(GridPass &pass) noexcept {
        pass.clearTiles();
        pass.clearCache();
        m_presented.fill(false);
        m_submitted_chunks = 0u;
    }

    std::error_code ColonyTranslator::markChunkChanged(const sim::ChunkPos pos) noexcept {
        if (pos.m_x >= sim::kChunksPerEdge or
            pos.m_y >= sim::kChunksPerEdge)
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_presented[sim::chunkIndexOf(pos)] = false;
        return {};
    }

    std::error_code ColonyTranslator::submit(const sim::ChunkGrid &grid, GridPass &pass) {
        const hal::ProfileScope scope_submit{"Colony.Submit"};
        const StageTimer timer_submit{m_timings, Stage::Submit};
        Array<GridTileSubmission> tiles{};
        tiles.reserve(sim::kChunkCount);
        {
            const hal::ProfileScope scope_gather{"Colony.Submit.Gather"};
            for (u32 y = 0u; y < sim::kChunksPerEdge; ++y) {
                for (u32 x = 0u; x < sim::kChunksPerEdge; ++x) {
                    const sim::ChunkPos pos{x, y};
                    if (not grid.isResident(pos)) {
                        continue;
                    }
                    const u32 chunk_id = sim::chunkIndexOf(pos);
                    tiles.push_back(GridTileSubmission{
                        .m_chunk_id = chunk_id,
                        .m_tile_range = tileRange(pos),
                        .m_dirty_mask = m_presented[chunk_id] ? 0u : 1u,
                        .m_material_id = 0u,
                    });
                }
            }
        }

        {
            const hal::ProfileScope scope_tiles{"Colony.Submit.Tiles"};
            if (const std::error_code error = pass.submitTiles(tiles)) [[unlikely]] {
                reset(pass);
                return error;
            }
        }

        Array<u16> scratch{};
        {
            const hal::ProfileScope scope_stage{"Colony.Submit.Stage"};
            for (const GridTileSubmission &tile: tiles) {
                if (tile.m_dirty_mask == 0u) {
                    continue;
                }
                const sim::ChunkPos pos{tile.m_chunk_id % sim::kChunksPerEdge, tile.m_chunk_id / sim::kChunksPerEdge};
                if (const std::error_code error = uploadChunk(grid, pass, pos, scratch)) [[unlikely]] {
                    reset(pass);
                    return error;
                }
            }
        }

        for (const GridTileSubmission &tile: tiles) {
            m_presented[tile.m_chunk_id] = true;
        }
        m_submitted_chunks = static_cast<u32>(tiles.size());
        return {};
    }

    u32 ColonyTranslator::submittedChunks() const noexcept {
        return m_submitted_chunks;
    }

    const StageTimings& ColonyTranslator::stageTimings() const noexcept {
        return m_timings;
    }

    void ColonyTranslator::resetStageTimings() noexcept {
        m_timings = StageTimings{};
    }
}
