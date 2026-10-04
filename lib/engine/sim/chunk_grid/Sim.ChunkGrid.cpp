module;
#include "pP/Macros.h"

module engine.sim;

import :chunk_grid;

import engine.core;
import engine.math;

import std;

namespace pP::sim {
    ChunkGrid::ChunkGrid() {
        m_states.resize(kChunkCount);
        m_payloads.resize(kChunkCount);
    }

    std::error_code ChunkGrid::setCell(const GlobalCellPos pos, const Cell cell) {
        if (not isInsideWorld(pos)) [[unlikely]] {
            return make_error_code(std::errc::result_out_of_range);
        }

        if (not isFinite<float>(cell.m_temperature)) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        const ChunkPos chunk = chunkOf(pos);
        const LocalCellPos local = localOf(pos);
        cellAt(pos) = cell;

        ChunkState &state = m_states[chunkIndexOf(chunk)];
        if (not state.m_dirty) {
            state.m_min_x = static_cast<u16>(local.m_x);
            state.m_min_y = static_cast<u16>(local.m_y);
            state.m_max_x = static_cast<u16>(local.m_x);
            state.m_max_y = static_cast<u16>(local.m_y);
        } else {
            state.m_min_x = std::min(state.m_min_x, static_cast<u16>(local.m_x));
            state.m_min_y = std::min(state.m_min_y, static_cast<u16>(local.m_y));
            state.m_max_x = std::max(state.m_max_x, static_cast<u16>(local.m_x));
            state.m_max_y = std::max(state.m_max_y, static_cast<u16>(local.m_y));
        }
        state.m_dirty = true;

        return {};
    }

    std::error_code ChunkGrid::setCells(const ChunkPos pos, const std::span<const Cell> cells) {
        PPR_ASSERT(isInsideChunkGrid(pos));

        if (cells.size() != kCellsPerChunk) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        for (const Cell &cell: cells) {
            if (not isFinite<float>(cell.m_temperature)) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
        }

        materialize(pos);

        std::ranges::copy(cells, (*m_payloads[chunkIndexOf(pos)]).begin());

        ChunkState &state = m_states[chunkIndexOf(pos)];
        state.m_dirty = true;
        state.m_min_x = 0u;
        state.m_min_y = 0u;
        state.m_max_x = static_cast<u16>(kChunkEdge - 1u);
        state.m_max_y = static_cast<u16>(kChunkEdge - 1u);
        return {};
    }

    std::error_code ChunkGrid::setCells(const ChunkPos pos, Array<Cell> &&cells) {
        PPR_ASSERT(isInsideChunkGrid(pos));

        if (cells.size() != kCellsPerChunk) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        for (const Cell &cell: cells) {
            if (not isFinite<float>(cell.m_temperature)) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
        }

        m_payloads[chunkIndexOf(pos)] = std::move(cells);

        ChunkState &state = m_states[chunkIndexOf(pos)];
        state.m_dirty = true;
        state.m_min_x = 0u;
        state.m_min_y = 0u;
        state.m_max_x = static_cast<u16>(kChunkEdge - 1u);
        state.m_max_y = static_cast<u16>(kChunkEdge - 1u);
        return {};
    }

    Expected<Cell> ChunkGrid::getCell(const GlobalCellPos pos) const {
        if (not isInsideWorld(pos)) [[unlikely]] {
            return std::unexpected{make_error_code(std::errc::result_out_of_range)};
        }

        const ChunkPos chunk = chunkOf(pos);
        const std::optional<Array<Cell>> &payload = m_payloads[chunkIndexOf(chunk)];
        if (not payload.has_value()) {
            return Cell{};
        }

        const LocalCellPos local = localOf(pos);
        return (*payload)[local.m_y * kChunkEdge + local.m_x];
    }

    std::optional<std::span<const Cell>> ChunkGrid::residentCells(const ChunkPos pos) const noexcept {
        PPR_ASSERT(isInsideChunkGrid(pos));

        const std::optional<Array<Cell>> &payload = m_payloads[chunkIndexOf(pos)];
        if (not payload.has_value()) {
            return std::nullopt;
        }

        return std::span<const Cell>{*payload};
    }

    bool ChunkGrid::isResident(const ChunkPos pos) const noexcept {
        PPR_ASSERT(isInsideChunkGrid(pos));

        return m_payloads[chunkIndexOf(pos)].has_value();
    }

    void ChunkGrid::markDirty(const ChunkPos pos) {
        PPR_ASSERT(isInsideChunkGrid(pos));

        materialize(pos);

        ChunkState &state = m_states[chunkIndexOf(pos)];
        state.m_dirty = true;
        state.m_min_x = 0u;
        state.m_min_y = 0u;
        state.m_max_x = static_cast<u16>(kChunkEdge - 1u);
        state.m_max_y = static_cast<u16>(kChunkEdge - 1u);
    }

    void ChunkGrid::clearDirty(const ChunkPos pos) noexcept {
        PPR_ASSERT(isInsideChunkGrid(pos));

        ChunkState &state = m_states[chunkIndexOf(pos)];
        state.m_dirty = false;
        state.m_min_x = 0u;
        state.m_min_y = 0u;
        state.m_max_x = 0u;
        state.m_max_y = 0u;
    }

    void ChunkGrid::clearAllDirty() noexcept {
        for (ChunkState &state: m_states) {
            state.m_dirty = false;
            state.m_min_x = 0u;
            state.m_min_y = 0u;
            state.m_max_x = 0u;
            state.m_max_y = 0u;
        }
    }

    EChunkActivity ChunkGrid::activity(const ChunkPos pos) const noexcept {
        PPR_ASSERT(isInsideChunkGrid(pos));

        return m_states[chunkIndexOf(pos)].m_activity;
    }

    void ChunkGrid::setActivity(const ChunkPos pos, const EChunkActivity activity) noexcept {
        PPR_ASSERT(isInsideChunkGrid(pos));

        m_states[chunkIndexOf(pos)].m_activity = activity;
    }

    Array<ChunkPos> ChunkGrid::dirtyChunks() const {
        return gatherDirty(false);
    }

    Array<ChunkPos> ChunkGrid::activeDirtyChunks() const {
        return gatherDirty(true);
    }

    u32 ChunkGrid::residentChunkCount() const noexcept {
        return safe_narrowing(std::ranges::count_if(m_payloads, [](const std::optional<Array<Cell>> &payload) {
            return payload.has_value();
        }));
    }

    u32 ChunkGrid::dirtyChunkCount() const noexcept {
        return safe_narrowing(std::ranges::count_if(m_states, [](const ChunkState &state) { return state.m_dirty; }));
    }

    std::optional<DirtyBounds> ChunkGrid::dirtyBounds(const ChunkPos pos) const noexcept {
        PPR_ASSERT(isInsideChunkGrid(pos));

        const ChunkState &state = m_states[chunkIndexOf(pos)];
        if (not state.m_dirty) {
            return std::nullopt;
        }

        return DirtyBounds{
            static_cast<u32>(state.m_min_x), static_cast<u32>(state.m_min_y), static_cast<u32>(state.m_max_x),
            static_cast<u32>(state.m_max_y)};
    }

    Array<ChunkPos> ChunkGrid::gatherDirty(const bool activeOnly) const {
        Array<ChunkPos> dirty{};

        for (u32 index = 0u; index < kChunkCount; ++index) {
            const ChunkState &state = m_states[index];
            if (not state.m_dirty) {
                continue;
            }

            if (activeOnly and state.m_activity != EChunkActivity::active) {
                continue;
            }

            dirty.push_back(ChunkPos{index % kChunksPerEdge, index / kChunksPerEdge});
        }

        return dirty;
    }

    void ChunkGrid::materialize(const ChunkPos pos) {
        std::optional<Array<Cell>> &payload = m_payloads[chunkIndexOf(pos)];
        if (payload.has_value()) {
            return;
        }

        payload.emplace().resize(kCellsPerChunk);
    }

    Cell &ChunkGrid::cellAt(const GlobalCellPos pos) {
        const ChunkPos chunk = chunkOf(pos);
        materialize(chunk);

        const LocalCellPos local = localOf(pos);
        return (*m_payloads[chunkIndexOf(chunk)])[local.m_y * kChunkEdge + local.m_x];
    }
}
