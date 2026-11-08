module;
#include "pP/Macros.h"

module engine.app;

import :renderer.world_light_source;
import :renderer.world_light_types;

import engine.core;
import std;

namespace pP::lighting {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(WorldLightSource, info, none)

    // ------------------------------------------------------------------
    // dense world source
    // ------------------------------------------------------------------

    namespace {
        inline constexpr u32 kChunkEdge = 128u;
        inline constexpr u64 kChunkCellCount = u64{kChunkEdge} * kChunkEdge;

        std::error_code require_(const bool valid, const std::errc error = std::errc::invalid_argument) noexcept {
            return valid ? std::error_code{} : std::make_error_code(error);
        }
    }

    Expected<WorldLightSource> WorldLightSource::create(
        WorldLightPlan plan, const std::span<const WorldLightMaterial> materials) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLightSource, require_(plan.m_width != 0u and plan.m_height != 0u));

        const u64 cell_count = u64{plan.m_width} * plan.m_height;

        WorldLightSource source;
        source.m_plan = std::move(plan);
        source.m_materials.assign(materials.begin(), materials.end());
        source.m_cell_ids.assign(cell_count, 0u);
        source.m_overlay.assign(cell_count, WorldLightOverlayCell{});

        source.m_blocks_x = (source.m_plan.m_width + kOccupancyBlockEdge - 1u) / kOccupancyBlockEdge;
        source.m_blocks_y = (source.m_plan.m_height + kOccupancyBlockEdge - 1u) / kOccupancyBlockEdge;
        source.m_words_per_row = (source.m_blocks_x + 31u) / 32u;
        source.m_occupancy.assign(u64{source.m_blocks_y} * source.m_words_per_row, 0u);

        return source;
    }

    std::error_code WorldLightSource::applyUpdate(const WorldLightSourceUpdate &update) {
        const u32 width = m_plan.m_width;
        const u32 height = m_plan.m_height;
        const u32 chunks_x = (width + kChunkEdge - 1u) / kChunkEdge;
        const u32 chunks_y = (height + kChunkEdge - 1u) / kChunkEdge;
        const u64 chunk_count = u64{chunks_x} * chunks_y;

        // Validate the whole edit before mutating anything so a rejected update
        // retains the previous ids, overlay, revision and dirty rect.
        for (const WorldLightChunkUpdate &chunk: update.m_chunks) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSource, require_(chunk.m_chunk_id < chunk_count, std::errc::result_out_of_range));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSource, require_(chunk.m_cell_ids.size() == kChunkCellCount));
        }
        for (const WorldLightOverlayCell &cell: update.m_overlay) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSource, require_(cell.m_cell < m_cell_ids.size(), std::errc::result_out_of_range));
        }

        bool changed = false;
        WorldLightDirtyRect dirty;
        std::vector<u32> dirty_blocks;

        for (const WorldLightChunkUpdate &chunk: update.m_chunks) {
            const u32 chunk_x = chunk.m_chunk_id % chunks_x;
            const u32 chunk_y = chunk.m_chunk_id / chunks_x;
            const u32 cell_x0 = chunk_x * kChunkEdge;
            const u32 cell_y0 = chunk_y * kChunkEdge;
            const u32 cell_x1 = std::min(cell_x0 + kChunkEdge, width);
            const u32 cell_y1 = std::min(cell_y0 + kChunkEdge, height);

            bool chunk_changed = false;
            for (u32 y = cell_y0; y < cell_y1; ++y) {
                for (u32 x = cell_x0; x < cell_x1; ++x) {
                    const u32 local = (y - cell_y0) * kChunkEdge + (x - cell_x0);
                    const u16 id = chunk.m_cell_ids[local];
                    u16 &slot = m_cell_ids[u64{y} * width + x];
                    if (slot != id) {
                        slot = id;
                        chunk_changed = true;
                    }
                }
            }

            if (not chunk_changed) {
                continue;
            }

            changed = true;
            dirty.include(
                static_cast<i32>(cell_x0), static_cast<i32>(cell_y0), static_cast<i32>(cell_x1),
                static_cast<i32>(cell_y1));

            // Collect the blocks this chunk touches for the occupancy recompute.
            const u32 block_x0 = cell_x0 / kOccupancyBlockEdge;
            const u32 block_y0 = cell_y0 / kOccupancyBlockEdge;
            const u32 block_x1 = (cell_x1 + kOccupancyBlockEdge - 1u) / kOccupancyBlockEdge;
            const u32 block_y1 = (cell_y1 + kOccupancyBlockEdge - 1u) / kOccupancyBlockEdge;
            for (u32 by = block_y0; by < block_y1; ++by) {
                for (u32 bx = block_x0; bx < block_x1; ++bx) {
                    dirty_blocks.push_back(by * m_blocks_x + bx);
                }
            }
        }

        for (const WorldLightOverlayCell &cell: update.m_overlay) {
            WorldLightOverlayCell &slot = m_overlay[cell.m_cell];
            const bool same = slot.m_coverage == cell.m_coverage and
                              slot.m_sigma == cell.m_sigma and
                              slot.m_emission == cell.m_emission;
            if (same) {
                continue;
            }

            slot = cell;
            changed = true;
            const u32 x = cell.m_cell % width;
            const u32 y = cell.m_cell / width;
            dirty.include(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(x) + 1, static_cast<i32>(y) + 1);
            dirty_blocks.push_back((y / kOccupancyBlockEdge) * m_blocks_x + (x / kOccupancyBlockEdge));
        }

        if (update.m_sky != m_sky or update.m_sky_hour != m_sky_hour) {
            m_sky = update.m_sky;
            m_sky_hour = update.m_sky_hour;
            changed = true;
        }

        if (not changed) {
            return default_value_v;
        }

        // Recompute occupancy for every touched block once, after ids and overlay
        // are both applied, so overlay-only cells set their bit and zeroed rows
        // clear their overlay contribution.
        std::sort(dirty_blocks.begin(), dirty_blocks.end());
        dirty_blocks.erase(std::unique(dirty_blocks.begin(), dirty_blocks.end()), dirty_blocks.end());
        for (const u32 block: dirty_blocks) {
            recomputeOccupancyBlock_(block % m_blocks_x, block / m_blocks_x);
        }

        m_dirty = dirty;
        m_revision = update.m_revision;
        return default_value_v;
    }

    u16 WorldLightSource::cellId(const u32 x, const u32 y) const noexcept {
        if (x >= m_plan.m_width or y >= m_plan.m_height) {
            return 0u;
        }
        return m_cell_ids[u64{y} * m_plan.m_width + x];
    }

    WorldLightOverlayCell WorldLightSource::overlayCell(const u32 x, const u32 y) const noexcept {
        if (x >= m_plan.m_width or y >= m_plan.m_height) {
            return WorldLightOverlayCell{};
        }
        return m_overlay[u64{y} * m_plan.m_width + x];
    }

    u64 WorldLightSource::revision() const noexcept {
        return m_revision;
    }

    WorldLightDirtyRect WorldLightSource::dirtyRect() const noexcept {
        return m_dirty;
    }

    u32 WorldLightSource::occupancyWord(const u32 block, const u32 word) const noexcept {
        if (block >= m_blocks_y or word >= m_words_per_row) {
            return 0u;
        }
        return m_occupancy[u64{block} * m_words_per_row + word];
    }

    void WorldLightSource::recomputeOccupancyBlock_(const u32 block_x, const u32 block_y) noexcept {
        const u32 width = m_plan.m_width;
        const u32 height = m_plan.m_height;
        const u32 cx0 = block_x * kOccupancyBlockEdge;
        const u32 cy0 = block_y * kOccupancyBlockEdge;
        const u32 cx1 = std::min(cx0 + kOccupancyBlockEdge, width);
        const u32 cy1 = std::min(cy0 + kOccupancyBlockEdge, height);

        u32 occupied = 0u;
        for (u32 cy = cy0; cy < cy1 and occupied == 0u; ++cy) {
            for (u32 cx = cx0; cx < cx1; ++cx) {
                const u64 index = u64{cy} * width + cx;
                if (m_cell_ids[index] != 0u) {
                    occupied = 1u;
                    break;
                }

                const WorldLightOverlayCell &overlay = m_overlay[index];
                const bool overlay_occupied = overlay.m_coverage > 0.0f or
                                              overlay.m_sigma[0] > 0.0f or
                                              overlay.m_sigma[1] > 0.0f or
                                              overlay.m_sigma[2] > 0.0f or
                                              overlay.m_emission[0] > 0.0f or
                                              overlay.m_emission[1] > 0.0f or
                                              overlay.m_emission[2] > 0.0f;
                if (overlay_occupied) {
                    occupied = 1u;
                    break;
                }
            }
        }

        u32 &word = m_occupancy[u64{block_y} * m_words_per_row + block_x / 32u];
        const u32 mask = 1u << (block_x % 32u);
        if (occupied != 0u) {
            word |= mask;
        } else {
            word &= ~mask;
        }
    }
} // namespace pP::lighting
