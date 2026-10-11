module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.grid_pass;
import :renderer.frame_fence;
import :renderer.types;
import :scene.camera;
import std;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(GridPass, debug, none)

    namespace {
        // Never bind a bare rhi::Binding(): the default range does not survive
        // the module boundary and would build an empty SRV view. Every
        // setBinding below goes through makeFullRange.
        [[nodiscard]] rhi::BufferRange makeFullRange(rhi::IBuffer *const buffer) noexcept {
            return rhi::BufferRange{
                .offset = 0u,
                .size = buffer->getDesc().size,
            };
        }

        // Chunk-sized tiles are square in x/y by construction, so the scalar
        // Box size is exact in x/y (mango sizes by the full extent, hence the
        // doubling of the half-extent); the z slab is a conservative half
        // cell so grazing views never drop an in-frustum tile on depth.
        [[nodiscard]] Box tileBox_(const GridTileRange &range, const float cell_size_m) noexcept {
            const float center_x = (static_cast<float>(range.m_min_x) + static_cast<float>(range.m_max_x)) * 0.5f;
            const float center_y = (static_cast<float>(range.m_min_y) + static_cast<float>(range.m_max_y)) * 0.5f;
            const float half_x = (static_cast<float>(range.m_max_x) - static_cast<float>(range.m_min_x)) * 0.5f;
            const float half_y = (static_cast<float>(range.m_max_y) - static_cast<float>(range.m_min_y)) * 0.5f;
            const float half = std::max({half_x, half_y, 0.5f}) + GridPass::kGridCullEpsilon;
            return Box{float3{center_x * cell_size_m, center_y * cell_size_m, 0.0f}, half * 2.0f * cell_size_m};
        }

        void applySubmissionBounds_(GridTilePayload &payload, const GridTileRange &range) noexcept {
            payload.m_rect[0] = static_cast<float>(range.m_min_x);
            payload.m_rect[1] = static_cast<float>(range.m_min_y);
            payload.m_rect[2] = static_cast<float>(range.m_max_x);
            payload.m_rect[3] = static_cast<float>(range.m_max_y);
        }
    }

    namespace fs = std::filesystem;

    // ------------------------------------------------------------------
    // validation and planning
    // ------------------------------------------------------------------

    std::error_code GridPass::validateSubmission_(const GridTileSubmission &submission) noexcept {
        const GridTileRange &range = submission.m_tile_range;
        if (range.m_max_x <= range.m_min_x or range.m_max_y <= range.m_min_y) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (range.m_min_x < 0 or range.m_min_y < 0 or range.m_max_x > kGridWorldEdge or range.m_max_y > kGridWorldEdge)
            [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        // Chunk-sized tiles use bit 0 only; higher bits name tiles that do
        // not exist, so they fail closed instead of silently uploading nothing.
        if ((submission.m_dirty_mask & ~1u) != 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return default_value_v;
    }

    // Pure, CPU-only, and unit-tested without a device: keeps the submissions
    // whose epsilon-expanded tile boxes intersect the snapshot frustum, in
    // submission order. Quiescent/off-screen tiles never reach the plan.
    Expected<GridTilePlan> GridPass::planTiles(
        const CameraSnapshot &snapshot, const std::span<const GridTileSubmission> submissions,
        const float cell_size_m) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, (cell_size_m == 1.0f or cell_size_m == 1.0f / 3.0f)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        GridTilePlan plan{};
        for (std::size_t index = 0u; index < submissions.size(); ++index) {
            const GridTileSubmission &submission = submissions[index];
            if (const std::error_code err = validateSubmission_(submission)) [[unlikely]] {
                return std::unexpected{err};
            }
            if (not snapshot.m_frustum.isVisible(tileBox_(submission.m_tile_range, cell_size_m))) {
                continue;
            }
            plan.m_tiles.push_back(
                GridPlannedTile{
                    .m_submission_index = static_cast<u32>(index),
                    .m_chunk_id = submission.m_chunk_id,
                    .m_tile_range = submission.m_tile_range,
                    .m_material_id = submission.m_material_id,
                });
        }
        return plan;
    }

    // Pure, CPU-only, and unit-tested without a device: one indirect draw per
    // planned tile in plan order, so the args sequence IS the visible set.
    // Past the tile-cache budget the lane fails closed (no_buffer_space)
    // under the same policy as requestUpload, never a partial args buffer.
    // An empty plan compacts to zero args: zero tiles, zero draws.
    Expected<Array<rhi::IndirectDrawArguments>> GridPass::compactIndirect(const GridTilePlan &plan) {
        if (plan.m_tiles.size() > kGridTileCacheCapacity) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }

        Array<rhi::IndirectDrawArguments> args{};
        args.reserve(plan.m_tiles.size());
        for (std::size_t index = 0u; index < plan.m_tiles.size(); ++index) {
            args.push_back(
                rhi::IndirectDrawArguments{
                    .vertexCountPerInstance = kGridTileVertexCount,
                    .instanceCount = 1u,
                    .startVertexLocation = 0u,
                    .startInstanceLocation = safe_narrowing(index),
                });
        }
        return args;
    }

    // ------------------------------------------------------------------
    // pass lifecycle and submission APIs
    // ------------------------------------------------------------------

    std::error_code GridPass::initialize(
        IRhiService &rhi_service, IShaderService &shader_service, const fs::path &content_dir) {
        rhi::IDevice &device = rhi_service.getDevice();

        bool shader_state_started = false;
        bool initialization_complete = false;
        PPR_DEFER {
            if (not initialization_complete) {
                if (shader_state_started) {
                    m_cached_pipeline.reset();
                    m_render_pipeline_key.reset();
                    m_shader_program.setNull();
                }
                m_ready = false;
            }
        };

        shader_state_started = true;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, createShaderProgram_(shader_service, device, content_dir));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, createVisibilityPrograms_(shader_service, device, content_dir));

        m_ready = true;
        initialization_complete = true;

        PPR_LOG(GridPass, info, "GridPass initialized");
        return default_value_v;
    }

    std::error_code GridPass::update([[maybe_unused]] TimeSpan dt, const CameraSnapshot &camera_view) {
        m_visibility_prepared = false;
        m_camera_view = camera_view;

        return default_value_v;
    }

    std::error_code GridPass::submitTiles(const std::span<const GridTileSubmission> submissions) {
        m_visibility_prepared = false;
        // Pure CPU: staging needs no device, so device-free tests and the
        // colony translator can stage before (or without) initialize; only
        // render touches the GPU.
        for (const GridTileSubmission &submission: submissions) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSubmission_(submission));
        }
        const bool unchanged = std::ranges::equal(
            m_submitted_tiles, submissions, [](const GridTileSubmission &lhs, const GridTileSubmission &rhs) {
                return lhs.m_chunk_id == rhs.m_chunk_id and
                       lhs.m_material_id == rhs.m_material_id and
                       lhs.m_tile_range.m_min_x == rhs.m_tile_range.m_min_x and
                       lhs.m_tile_range.m_min_y == rhs.m_tile_range.m_min_y and
                       lhs.m_tile_range.m_max_x == rhs.m_tile_range.m_max_x and
                       lhs.m_tile_range.m_max_y == rhs.m_tile_range.m_max_y;
            });
        m_resident_table_stale = m_resident_table_stale or not unchanged;
        m_submitted_tiles.clear();
        m_submitted_tiles.reserve(submissions.size());
        for (const GridTileSubmission &submission: submissions) {
            m_submitted_tiles.push_back(submission);
        }
        return default_value_v;
    }

    void GridPass::clearTiles() noexcept {
        m_visibility_prepared = false;
        m_resident_table_stale = true;
        m_submitted_tiles.clear();
    }

    void GridPass::clearCache() noexcept {
        m_visibility_prepared = false;
        m_resident_table_stale = true;
        cancelCellCohort_();
        m_cell_slots.fill(std::nullopt);
        m_cell_slots_current.reset();
        m_cell_publication_deferred = false;
        m_tile_cache.clear();
        m_cell_cache.clear();
        // Keep the atlas allocated: its previous contents may still be read
        // by an in-flight draw. A future cell request repopulates its slot
        // only after its last submitted reader fence completes.
    }

    std::error_code GridPass::applyStagedFrame(const GridStagedFrame &stage) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, (stage.m_cell_size_m == 1.0f or stage.m_cell_size_m == 1.0f / 3.0f)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        m_cell_size_m = stage.m_cell_size_m;
        if (stage.m_reset_caches) {
            clearTiles();
            clearCache();
        }
        PPR_RETURN_ERROR_ON_FAIL(GridPass, submitTiles(std::span<const GridTileSubmission>{stage.m_tiles}));
        for (const GridStagedUpload &upload: stage.m_uploads) {
            GridUploadRequest request{};
            request.m_chunk_id = upload.m_chunk_id;
            request.m_tile_data_view = std::span<const GridTilePayload>{&upload.m_payload, 1u};
            request.m_cell_materials = std::span<const u16>{
                upload.m_cell_materials.data(), upload.m_cell_materials.size()};
            const Expected<u32> uploaded = requestUpload(request);
            if (not uploaded) [[unlikely]] {
                clearTiles();
                clearCache();
                return uploaded.error();
            }
            if (*uploaded != 1u) [[unlikely]] {
                clearTiles();
                clearCache();
                return std::make_error_code(std::errc::state_not_recoverable);
            }
        }
        return default_value_v;
    }

    Expected<u32> GridPass::requestUpload(const GridUploadRequest &request) {
        m_visibility_prepared = false;
        // Pure CPU like submitTiles: the tile cache is pass-owned CPU data,
        // so quiescent tiles keep serving through device loss while
        // GPU-touching calls park.
        const GridTileSubmission *staged = nullptr;
        for (const GridTileSubmission &submission: m_submitted_tiles) {
            if (submission.m_chunk_id == request.m_chunk_id) {
                staged = &submission;
                break;
            }
        }
        if (staged == nullptr) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        const u32 dirty_count = static_cast<u32>(std::popcount(staged->m_dirty_mask));
        const bool has_cells = not request.m_cell_materials.empty();
        if (request.m_tile_data_view.size() < dirty_count or
            (has_cells and request.m_cell_materials.size() != kCellsPerChunk)) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (has_cells) {
            const u32 chunk_x = request.m_chunk_id % (kGridWorldEdge / kGridTileEdge);
            const u32 chunk_y = request.m_chunk_id / (kGridWorldEdge / kGridTileEdge);
            const GridTileRange &range = staged->m_tile_range;
            if (request.m_chunk_id >= kGridTileCacheCapacity or
                range.m_min_x != static_cast<i32>(chunk_x * kGridTileEdge) or
                range.m_min_y != static_cast<i32>(chunk_y * kGridTileEdge) or
                range.m_max_x != static_cast<i32>((chunk_x + 1u) * kGridTileEdge) or
                range.m_max_y != static_cast<i32>((chunk_y + 1u) * kGridTileEdge)) [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
        }
        if (dirty_count == 0u) {
            // Quiescent chunk: nothing to upload, the cache keeps serving it.
            return 0u;
        }

        const bool is_new_entry = m_tile_cache.find(request.m_chunk_id) == m_tile_cache.end();
        if (is_new_entry and m_tile_cache.size() >= kGridTileCacheCapacity) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }

        // Pack before touching either cache. The low halfword is even cell i,
        // the high halfword is cell i+1 (including transparent 0xffff).
        std::unique_ptr<std::array<u32, kCellWordsPerChunk>> packed{};
        if (has_cells) {
            packed = std::make_unique<std::array<u32, kCellWordsPerChunk>>();
            for (u32 word = 0u; word < kCellWordsPerChunk; ++word) {
                (*packed)[word] = static_cast<u32>(request.m_cell_materials[word * 2u]) |
                                  (static_cast<u32>(request.m_cell_materials[word * 2u + 1u]) << 16u);
            }
        }

        // Chunk-sized tiles carry exactly one dirty tile (bit 0); the loop
        // stays general in tile-index order for a finer tiling that never
        // arrived, consuming one view element per set bit.
        u32 uploaded = 0u;
        for (u32 tile = 0u; tile < kTilesPerChunk; ++tile) {
            if ((staged->m_dirty_mask & (u64{1u} << tile)) == 0u) {
                continue;
            }
            GridCachedTile cached{};
            cached.m_payload = request.m_tile_data_view[uploaded];
            cached.m_material_id = staged->m_material_id;
            if (const auto found = m_tile_cache.find(request.m_chunk_id); found != m_tile_cache.end()) {
                found->second = cached;
            } else {
                m_tile_cache.emplace(request.m_chunk_id, cached);
            }
            ++uploaded;
        }

        m_upload_count += uploaded;
        m_resident_table_stale = true;
        if (has_cells) {
            const auto previous = m_cell_cache.find(request.m_chunk_id);
            // A deferred frame can replay the same unacknowledged revision.
            // Keep its partially copied cohort unless the actual cells change.
            if (previous == m_cell_cache.end() or *previous->second.m_words != *packed) {
                cancelCellCohort_();
                m_cell_cache.insert_or_assign(request.m_chunk_id, GridCachedCells{std::move(packed), true});
            }
        } else {
            cancelCellCohort_();
            m_cell_cache.erase(request.m_chunk_id);
            if (request.m_chunk_id < kGridTileCacheCapacity and m_cell_slots[request.m_chunk_id]) {
                m_cell_slots_current.reset(*m_cell_slots[request.m_chunk_id]);
                m_cell_slots[request.m_chunk_id].reset();
            }
        }
        return uploaded;
    }

    std::error_code GridPass::ensureCellBuffer_(rhi::IDevice &device, const bool atlas_required) {
        rhi::ComPtr<rhi::IBuffer> &buffer = atlas_required ? m_cell_atlas : m_cell_dummy;
        if (buffer != nullptr) {
            return default_value_v;
        }
        rhi::BufferDesc desc{};
        desc.size = atlas_required ? static_cast<u64>(kCellAtlasWords) * sizeof(u32) : sizeof(u32);
        desc.elementSize = sizeof(u32);
        desc.memoryType = rhi::MemoryType::Upload;
        desc.usage = rhi::BufferUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = atlas_required ? "grid cell material atlas" : "grid cell material dummy";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(desc, nullptr, buffer.writeRef()));
        if (atlas_required) {
            if (const std::error_code err = make_error_code(
                    device.mapBuffer(buffer.get(), rhi::CpuAccessMode::Write, &m_cell_atlas_mapped))) [[unlikely]] {
                buffer.setNull();
                return err;
            }
        }
        return default_value_v;
    }

    std::error_code GridPass::prepareCellUploads(
        rhi::IDevice &device, std23::function_ref<std::error_code()> wait_for_gpu_idle, const bool all_tiles) {
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        Array<u32> pending{};
        const auto append_pending = [&](const u32 chunk_id) {
            const auto found = m_cell_cache.find(chunk_id);
            if (found != m_cell_cache.end() and
                found->second.m_pending and
                std::find(pending.begin(), pending.end(), chunk_id) == pending.end()) {
                pending.push_back(chunk_id);
            }
        };
        if (all_tiles) {
            for (const GridTileSubmission &tile: m_submitted_tiles) {
                append_pending(tile.m_chunk_id);
            }
        } else {
            const Expected<GridTilePlan> plan = planTiles(
                m_camera_view, std::span<const GridTileSubmission>{m_submitted_tiles}, m_cell_size_m);
            PPR_RETURN_ERROR_ON_FAIL(GridPass, plan ? std::error_code{} : plan.error());
            for (const GridPlannedTile &tile: plan->m_tiles) {
                append_pending(tile.m_chunk_id);
            }
        }
        if (pending.empty()) {
            m_cell_publication_deferred = not m_cell_cohort_chunks.empty();
            return default_value_v;
        }

        // Once a reference draw needs an upload, finish the whole pending
        // cohort. Publishing only visible replacements could pin both banks
        // and prevent a later interactive cohort from making progress.
        for (const auto entry: m_cell_cache) {
            append_pending(entry.first);
        }

        // Explicit reference fixtures retain their synchronous preparation API.
        // The interactive lane uses tryPrepareCellUploads instead.
        ++m_atlas_serialization_count;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_gpu_idle());
        const auto prepared = prepareCellCohort_(device, pending, m_last_submitted_value, kGridTileCacheCapacity);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, prepared);
        m_cell_publication_deferred = not *prepared;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, *prepared ? std::error_code{} : std::make_error_code(std::errc::no_buffer_space));
        const bool table_ready = std::ranges::all_of(m_submitted_tiles, [this](const GridTileSubmission &tile) {
            const auto found = m_cell_cache.find(tile.m_chunk_id);
            return found == m_cell_cache.end() or not found->second.m_pending;
        });
        if (table_ready and m_resident_table_stale) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, rebuildResidentTable_());
        }
        return {};
    }

    void GridPass::cancelCellCohort_() noexcept {
        for (const u32 slot: m_cell_cohort_slots) {
            m_cell_slots_reserved.reset(slot);
        }
        m_cell_cohort_chunks.clear();
        m_cell_cohort_slots.clear();
        m_cell_cohort_cursor = 0u;
    }

    Expected<bool> GridPass::tryPrepareCellUploads(
        rhi::IDevice &device, const FenceCompletionValue completed, const u32 chunk_budget) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, chunk_budget > 0u and chunk_budget <= kGridTileCacheCapacity
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        Array<u32> pending;
        for (const auto entry: m_cell_cache) {
            if (entry.second.m_pending) {
                pending.push_back(entry.first);
            }
        }
        const auto prepared = prepareCellCohort_(device, pending, completed, chunk_budget);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, prepared);
        if (*prepared and m_resident_table_stale) {
            // Retire the old table's pins before accepting another cohort,
            // even if frame admission later rejects the draw. Reader fences
            // continue protecting every retired physical slot.
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, rebuildResidentTable_());
        }
        m_cell_publication_deferred = not *prepared;
        if (m_cell_publication_deferred) {
            ++m_atlas_deferred_count;
        }
        return *prepared;
    }

    Expected<bool> GridPass::prepareCellCohort_(
        rhi::IDevice &device, const std::span<const u32> chunks, const FenceCompletionValue completed,
        const u32 chunk_budget) {
        if (chunks.empty()) {
            return true;
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, chunks.size() <= kGridTileCacheCapacity ? std::error_code{}
                                                              : std::make_error_code(std::errc::no_buffer_space));
        if (m_cell_cohort_chunks.empty()) {
            Array<u32> slots;
            slots.reserve(chunks.size());
            for (u32 slot = 0u; slot < kCellAtlasSlots and slots.size() < chunks.size(); ++slot) {
                if (not m_cell_slots_current.test(slot) and
                    not m_resident_cell_slots.test(slot) and
                    not m_cell_slots_reserved.test(slot) and
                    slotFenceComplete(completed, m_cell_slot_values[slot])) {
                    slots.push_back(slot);
                }
            }
            if (slots.size() != chunks.size()) {
                return false;
            }
            // Allocate/copy all CPU state before reserving any physical slot.
            Array<u32> cohort;
            cohort.assign(chunks.begin(), chunks.end());
            m_cell_cohort_chunks = std::move(cohort);
            m_cell_cohort_slots = std::move(slots);
            for (const u32 slot: m_cell_cohort_slots) {
                m_cell_slots_reserved.set(slot);
            }
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, ensureCellBuffer_(device, true));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass,
            m_cell_atlas_mapped != nullptr ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        auto *const atlas = static_cast<u32 *>(m_cell_atlas_mapped);
        const u32 end = std::min<u32>(safe_narrowing(m_cell_cohort_chunks.size()), m_cell_cohort_cursor + chunk_budget);
        for (; m_cell_cohort_cursor < end; ++m_cell_cohort_cursor) {
            const u32 chunk_id = m_cell_cohort_chunks[m_cell_cohort_cursor];
            const u32 slot = m_cell_cohort_slots[m_cell_cohort_cursor];
            const auto found = m_cell_cache.find(chunk_id);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, found != m_cell_cache.end() and found->second.m_pending
                              ? std::error_code{}
                              : std::make_error_code(std::errc::state_not_recoverable));
            std::memcpy(
                atlas + static_cast<std::size_t>(slot) * kCellWordsPerChunk, found->second.m_words->data(),
                sizeof(u32) * kCellWordsPerChunk);
            m_atlas_uploaded_bytes += sizeof(u32) * kCellWordsPerChunk;
        }
        if (m_cell_cohort_cursor != m_cell_cohort_chunks.size()) {
            return false;
        }

        // Publish the complete mapping only after every chunk was copied.
        for (u32 index = 0u; index < m_cell_cohort_chunks.size(); ++index) {
            const u32 chunk = m_cell_cohort_chunks[index];
            const u32 slot = m_cell_cohort_slots[index];
            if (m_cell_slots[chunk]) {
                m_cell_slots_current.reset(*m_cell_slots[chunk]);
            }
            m_cell_slots[chunk] = slot;
            m_cell_slots_current.set(slot);
            m_cell_cache.find(chunk)->second.m_pending = false;
        }
        cancelCellCohort_();
        m_resident_table_stale = true;
        ++m_atlas_write_count;
        return true;
    }

    std::error_code GridPass::validateCellDraw_(const GridTilePlan &plan) const noexcept {
        for (const GridPlannedTile &tile: plan.m_tiles) {
            const auto found = m_cell_cache.find(tile.m_chunk_id);
            if (found != m_cell_cache.end() and (found->second.m_pending or m_cell_atlas == nullptr)) [[unlikely]] {
                return std::make_error_code(std::errc::operation_in_progress);
            }
        }
        return default_value_v;
    }

    void GridPass::applyCellPayload_(GridTilePayload &payload, const u32 chunk_id) const noexcept {
        if (m_cell_cache.find(chunk_id) != m_cell_cache.end()) {
            PPR_ASSERT(m_cell_slots[chunk_id].has_value());
            payload.m_cell_word_base = *m_cell_slots[chunk_id] * kCellWordsPerChunk;
            payload.m_flags = 1u;
        } else {
            payload.m_cell_word_base = 0u;
            payload.m_flags = 0u;
        }
        payload.m_pad = 0u;
    }

    Expected<GridTilePayload> GridPass::resolveCached(const u32 chunk_id) noexcept {
        const auto found = m_tile_cache.find(chunk_id);
        if (found == m_tile_cache.end()) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        ++m_cache_hit_count;
        return found->second.m_payload;
    }

    void GridPass::notifyFrameSubmitted(const FenceCompletionValue submit_value) noexcept {
        m_last_submitted_value = submit_value;
        m_tile_slot_values[m_last_used_slot] = submit_value;
        for (u32 slot = 0u; slot < kCellAtlasSlots; ++slot) {
            if (m_cell_slots_current.test(slot) or m_resident_cell_slots.test(slot)) {
                m_cell_slot_values[slot] = submit_value;
            }
        }
    }

    void GridPass::reclaimCompleted_(const FenceCompletionValue completed) noexcept {
        std::ignore = m_deferred_releases.reclaim(completed);
    }

    std::error_code GridPass::selectSlot_(const FenceCompletionValue completed, FenceWaitHook &wait_for_fence) {
        const std::span<const FenceCompletionValue> values{m_tile_slot_values.data(), m_tile_slot_values.size()};
        const u32 slot = selectRingSlot(completed, values, m_tile_ring_cursor);
        if (not slotFenceComplete(completed, m_tile_slot_values[slot])) [[unlikely]] {
            // Backstop only: 3 slots vs at most 2 outstanding submissions
            // means the scan above always finds a complete slot unless
            // completion is artificially delayed. Never overwrite early.
            PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_fence(m_tile_slot_values[slot]));
        }
        m_tile_ring_cursor = slot;
        m_last_used_slot = slot;
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // tiling encode
    // ------------------------------------------------------------------

    // Failure contract, phase by phase. This is NOT an atomic-frame guarantee:
    //   planning all-or-nothing. planTiles validates every staged submission
    //            before any encode, so one malformed range fails the whole call.
    //   upload   all-or-nothing. ensureTilePayloads_ grows the ring before any
    //            draw is encoded.
    //   encoding fail-closed single draw: the one instanced draw either encodes
    //            whole or the frame stays empty. A caller that needs
    //            whole-frame atomicity must discard the pass.
    std::error_code GridPass::render(const DrawContext &draw_context) {
        m_visibility_prepared = false;
        const hal::ProfileScope scope{"App.Pass.Grid"};
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        // Retire replaced pipelines/roots even on frames that encode
        // nothing: deferred objects must not outlive their completion.
        reclaimCompleted_(draw_context.m_completed_fence_value);
        if (m_submitted_tiles.empty()) {
            return default_value_v;
        }

        Expected<GridTilePlan> plan = planTiles(
            m_camera_view, std::span<const GridTileSubmission>{m_submitted_tiles.data(), m_submitted_tiles.size()},
            m_cell_size_m);
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }
        if (plan->m_tiles.empty()) {
            return default_value_v;
        }
        PPR_RETURN_ERROR_ON_FAIL(GridPass, validateCellDraw_(*plan));
        const bool has_cells = std::ranges::any_of(plan->m_tiles, [this](const GridPlannedTile &tile) {
            return m_cell_cache.find(tile.m_chunk_id) != m_cell_cache.end();
        });
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, has_cells));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, false));

        // Resolve payloads: cache hits serve quiescent tiles, while a tile
        // with no cache entry yet stages cold straight from its submission so
        // the CPU-staged path still draws before its first upload.
        Array<GridTilePayload> payloads{};
        payloads.reserve(plan->m_tiles.size());
        for (const GridPlannedTile &tile: plan->m_tiles) {
            Expected<GridTilePayload> cached = resolveCached(tile.m_chunk_id);
            if (cached.has_value()) {
                applySubmissionBounds_(*cached, tile.m_tile_range);
                applyCellPayload_(*cached, tile.m_chunk_id);
                payloads.push_back(*cached);
                continue;
            }
            GridTilePayload staged_payload{};
            staged_payload.m_rect[0] = static_cast<float>(tile.m_tile_range.m_min_x);
            staged_payload.m_rect[1] = static_cast<float>(tile.m_tile_range.m_min_y);
            staged_payload.m_rect[2] = static_cast<float>(tile.m_tile_range.m_max_x);
            staged_payload.m_rect[3] = static_cast<float>(tile.m_tile_range.m_max_y);
            staged_payload.m_material = tile.m_material_id;
            applyCellPayload_(staged_payload, tile.m_chunk_id);
            payloads.push_back(staged_payload);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureTilePayloads_(
                          draw_context.m_device, safe_narrowing(payloads.size()), draw_context.m_completed_fence_value,
                          draw_context.m_wait_for_fence));

        // One rotation per submit onto a completion-selected slot: the draws
        // encoded after this point read the selected slot, whose last use
        // the fence has passed (or was waited out). The slot stays mapped
        // for its whole life, so there is no per-frame map/unmap pair. The
        // rotation is per render() submit and assumes a SINGLE
        // GridPass::render per frame (backpressured by acquireNextImage).
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, selectSlot_(draw_context.m_completed_fence_value, draw_context.m_wait_for_fence));
        void *const mapped_payloads = m_tile_payload_mapped[m_tile_ring_cursor];
        if (mapped_payloads == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        std::memcpy(
            mapped_payloads, payloads.data(), static_cast<std::size_t>(payloads.size()) * sizeof(GridTilePayload));
        // The reference lane shares storage with the persistent GPU table.
        m_resident_table_uploaded[m_tile_ring_cursor] = false;

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .indexBuffer = {},
        });

        rhi::IBuffer *const payload_buffer = m_tile_payloads[m_tile_ring_cursor].get();
        if (payload_buffer == nullptr or payload_buffer->getDesc().elementSize != sizeof(GridTilePayload))
            [[unlikely]] {
            PPR_LOG(
                GridPass, error, "tile payload stride mismatch vs shader expectation",
                {
                    {"payload_stride", payload_buffer == nullptr ? 0u : payload_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<GridCachedPipeline *> cached = pipelineFor_(draw_context.m_device, draw_context.m_render_pipeline_key);
        if (not cached.has_value()) [[unlikely]] {
            return cached.error();
        }
        rhi::IRenderPipeline *const pipeline = (*cached)->m_pipeline.get();
        rhi::IShaderObject *const root_object = (*cached)->m_slot_roots[m_tile_ring_cursor].get();
        rhi::ShaderCursor &frame_cursor = (*cached)->m_slot_frame_cursors[m_tile_ring_cursor];
        if (pipeline == nullptr or root_object == nullptr or not frame_cursor.isValid()) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // Per-slot root object: bound as-is; this frame's slot cursor was
        // resolved at creation, so only the data below is written — and only
        // into a slot no in-flight submission still reads.
        draw_context.m_pass.bindPipeline(pipeline, root_object);
        rhi::ShaderCursor shader_cursor{root_object};

        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_(frame_cursor));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_visible_ids"].setBinding(
                          rhi::Binding(m_cell_dummy.get(), makeFullRange(m_cell_dummy.get()))));

        // SV_InstanceID is draw-local on D3D12, SPIR-V, and Metal alike, so the
        // startInstanceLocation never reaches the shader. The tile base rides
        // the vertex entry's uniform param instead and the draw starts at
        // instance 0; binding both would double-count the offset.
        const u32 tile_base = 0u;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, shader_cursor["g_tile_base"].setData(&tile_base, sizeof(tile_base)));

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_tiles"].setBinding(rhi::Binding(payload_buffer, makeFullRange(payload_buffer))));
        rhi::IBuffer *const cell_buffer = has_cells ? m_cell_atlas.get() : m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            shader_cursor["g_cell_materials"].setBinding(rhi::Binding(cell_buffer, makeFullRange(cell_buffer))));

        draw_context.m_pass.draw({
            .vertexCount = 6u,
            .instanceCount = safe_narrowing(payloads.size()),
            .startInstanceLocation = 0u,
        });

        return default_value_v;
    }

    // Additive MDI lane beside render(): plan -> resolve payloads (cache hit,
    // or stage cold from the submission) -> upload tile ring plus the
    // compacted args ring -> one drawIndirect over the args. The CPU-staged
    // drawInstanced path above is untouched. Empty or fully-culled input
    // encodes zero draws and returns before any m_pass call.
    //
    // Offset discipline: payloads compact in plan order from slot 0 and
    // g_tile_base stays 0 for the whole call — the uniform cannot vary per
    // draw inside one indirect call — so each arg carries its tile's
    // first-payload offset as startInstanceLocation instead. Whether the
    // shader receives that offset as SV_InstanceID is backend-dependent and
    // MUST be verified by an MDI GPU readback parity test; do not infer parity
    // from the CPU-side args alone.
    std::error_code GridPass::renderIndirect(const DrawContext &draw_context) {
        m_visibility_prepared = false;
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        reclaimCompleted_(draw_context.m_completed_fence_value);
        if (m_submitted_tiles.empty()) {
            return default_value_v;
        }

        Expected<GridTilePlan> plan = planTiles(
            m_camera_view, std::span<const GridTileSubmission>{m_submitted_tiles.data(), m_submitted_tiles.size()},
            m_cell_size_m);
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }
        if (plan->m_tiles.empty()) {
            return default_value_v;
        }
        PPR_RETURN_ERROR_ON_FAIL(GridPass, validateCellDraw_(*plan));
        const bool has_cells = std::ranges::any_of(plan->m_tiles, [this](const GridPlannedTile &tile) {
            return m_cell_cache.find(tile.m_chunk_id) != m_cell_cache.end();
        });
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, has_cells));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(draw_context.m_device, false));

        Expected<Array<rhi::IndirectDrawArguments>> args = compactIndirect(*plan);
        if (not args.has_value()) [[unlikely]] {
            return args.error();
        }

        // Same resolve rule as the CPU path: cache hits serve quiescent
        // tiles, while a tile with no cache entry yet stages cold straight
        // from its submission.
        Array<GridTilePayload> payloads{};
        payloads.reserve(plan->m_tiles.size());
        for (const GridPlannedTile &tile: plan->m_tiles) {
            Expected<GridTilePayload> cached = resolveCached(tile.m_chunk_id);
            if (cached.has_value()) {
                applySubmissionBounds_(*cached, tile.m_tile_range);
                applyCellPayload_(*cached, tile.m_chunk_id);
                payloads.push_back(*cached);
                continue;
            }
            GridTilePayload staged_payload{};
            staged_payload.m_rect[0] = static_cast<float>(tile.m_tile_range.m_min_x);
            staged_payload.m_rect[1] = static_cast<float>(tile.m_tile_range.m_min_y);
            staged_payload.m_rect[2] = static_cast<float>(tile.m_tile_range.m_max_x);
            staged_payload.m_rect[3] = static_cast<float>(tile.m_tile_range.m_max_y);
            staged_payload.m_material = tile.m_material_id;
            applyCellPayload_(staged_payload, tile.m_chunk_id);
            payloads.push_back(staged_payload);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureTilePayloads_(
                          draw_context.m_device, safe_narrowing(payloads.size()), draw_context.m_completed_fence_value,
                          draw_context.m_wait_for_fence));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureIndirectArgs_(
                          draw_context.m_device, safe_narrowing(args->size()), draw_context.m_completed_fence_value,
                          draw_context.m_wait_for_fence));

        // One rotation per submit, shared with the payload ring and covered
        // by the single completion array: the draws encoded after this point
        // read the selected slot in BOTH rings, which no in-flight frame is
        // still reading. Both slots stay mapped for their whole life, so
        // there is no per-frame map/unmap pair.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, selectSlot_(draw_context.m_completed_fence_value, draw_context.m_wait_for_fence));
        void *const mapped_payloads = m_tile_payload_mapped[m_tile_ring_cursor];
        void *const mapped_args = m_indirect_args_mapped[m_tile_ring_cursor];
        if (mapped_payloads == nullptr or mapped_args == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        std::memcpy(
            mapped_payloads, payloads.data(), static_cast<std::size_t>(payloads.size()) * sizeof(GridTilePayload));
        m_resident_table_uploaded[m_tile_ring_cursor] = false;
        std::memcpy(
            mapped_args, args->data(), static_cast<std::size_t>(args->size()) * sizeof(rhi::IndirectDrawArguments));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .indexBuffer = {},
        });

        rhi::IBuffer *const payload_buffer = m_tile_payloads[m_tile_ring_cursor].get();
        if (payload_buffer == nullptr or payload_buffer->getDesc().elementSize != sizeof(GridTilePayload))
            [[unlikely]] {
            PPR_LOG(
                GridPass, error, "tile payload stride mismatch vs shader expectation",
                {
                    {"payload_stride", payload_buffer == nullptr ? 0u : payload_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        rhi::IBuffer *const args_buffer = m_indirect_args[m_tile_ring_cursor].get();
        if (args_buffer == nullptr or args_buffer->getDesc().elementSize != sizeof(rhi::IndirectDrawArguments))
            [[unlikely]] {
            PPR_LOG(
                GridPass, error, "indirect args stride mismatch vs drawIndirect expectation",
                {
                    {"args_stride", args_buffer == nullptr ? 0u : args_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<GridCachedPipeline *> cached = pipelineFor_(draw_context.m_device, draw_context.m_render_pipeline_key);
        if (not cached.has_value()) [[unlikely]] {
            return cached.error();
        }
        rhi::IRenderPipeline *const pipeline = (*cached)->m_pipeline.get();
        rhi::IShaderObject *const root_object = (*cached)->m_slot_roots[m_tile_ring_cursor].get();
        rhi::ShaderCursor &frame_cursor = (*cached)->m_slot_frame_cursors[m_tile_ring_cursor];
        if (pipeline == nullptr or root_object == nullptr or not frame_cursor.isValid()) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // Same bindings as the CPU path: this frame's slot root rebound
        // as-is, the frame constants, g_tile_base 0 (per-draw bases ride the
        // args, see above), and the payload buffer.
        draw_context.m_pass.bindPipeline(pipeline, root_object);
        rhi::ShaderCursor shader_cursor{root_object};

        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_(frame_cursor));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_visible_ids"].setBinding(
                          rhi::Binding(m_cell_dummy.get(), makeFullRange(m_cell_dummy.get()))));

        const u32 tile_base = 0u;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, shader_cursor["g_tile_base"].setData(&tile_base, sizeof(tile_base)));

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, shader_cursor["g_tiles"].setBinding(rhi::Binding(payload_buffer, makeFullRange(payload_buffer))));
        rhi::IBuffer *const cell_buffer = has_cells ? m_cell_atlas.get() : m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            shader_cursor["g_cell_materials"].setBinding(rhi::Binding(cell_buffer, makeFullRange(cell_buffer))));

        draw_context.m_pass.drawIndirect(safe_narrowing(args->size()), rhi::BufferOffsetPair{args_buffer});

        return default_value_v;
    }

    std::error_code GridPass::ensureTilePayloads_(
        rhi::IDevice &device, const u32 tile_count, const FenceCompletionValue completed,
        FenceWaitHook &wait_for_fence) {
        if (tile_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 payload_bytes = static_cast<u64>(tile_count) * sizeof(GridTilePayload);
        if (m_tile_payloads[0] != nullptr and m_tile_payload_capacity >= payload_bytes) {
            return default_value_v;
        }

        // Growth or first use: the whole ring is replaced, so every
        // submission that may still read it must complete first. Waiting for
        // the youngest tracked use covers all three slots at once; the wait
        // is skipped when nothing was ever submitted, or when the frame's
        // polled completion already passes every tracked use. Growth MUST
        // wait — unfenced replacement here was the highest-severity reuse
        // hole (R6).
        {
            const std::span<const FenceCompletionValue> values{m_tile_slot_values.data(), m_tile_slot_values.size()};
            const FenceCompletionValue youngest = maxSlotValue(values);
            if (youngest != kFenceValueNone and youngest > completed) {
                ++m_growth_wait_count;
                PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_fence(youngest));
            }
        }

        // Drop the whole ring (and unmap it, since a device is still
        // available here) before rebuilding it at the new size. The wait
        // above drained every tracked submission, so the shared completion
        // array restarts empty alongside the fresh buffers.
        releaseTilePayloads_(std::addressof(device));
        m_tile_slot_values.fill(kFenceValueNone);

        rhi::BufferDesc payload_desc{};
        payload_desc.size = payload_bytes;
        payload_desc.elementSize = sizeof(GridTilePayload);
        payload_desc.memoryType = rhi::MemoryType::Upload;
        payload_desc.usage = rhi::BufferUsage::ShaderResource;
        payload_desc.defaultState = rhi::ResourceState::ShaderResource;
        payload_desc.label = "grid tile payloads";
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (const std::error_code err = make_error_code(
                    device.createBuffer(payload_desc, nullptr, m_tile_payloads[slot].writeRef()))) [[unlikely]] {
                releaseTilePayloads_(std::addressof(device));
                return err;
            }
            if (const std::error_code err = make_error_code(device.mapBuffer(
                    m_tile_payloads[slot].get(), rhi::CpuAccessMode::Write,
                    std::addressof(m_tile_payload_mapped[slot])))) [[unlikely]] {
                releaseTilePayloads_(std::addressof(device));
                return err;
            }
        }

        m_tile_payload_capacity = payload_bytes;
        return default_value_v;
    }

    void GridPass::releaseTilePayloads_(rhi::IDevice *const device) noexcept {
        m_resident_table_uploaded.fill(false);
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (m_tile_payloads[slot] == nullptr) {
                m_tile_payload_mapped[slot] = nullptr;
                continue;
            }

            // Unmap only when a device survived: shutdown and device loss have
            // none to ask, and leaving those buffers mapped is safe because
            // SLANG_RHI_DEBUG_ENABLE_BUFFER_MAP_VALIDATION defaults to 0, so a
            // still-mapped release does not poison the debug layer.
            if (device != nullptr) {
                std::ignore = device->unmapBuffer(m_tile_payloads[slot].get());
            }
            m_tile_payloads[slot].setNull();
            m_tile_payload_mapped[slot] = nullptr;
        }
        m_tile_payload_capacity = 0u;
        m_tile_ring_cursor = 0u;
    }

    std::error_code GridPass::ensureIndirectArgs_(
        rhi::IDevice &device, const u32 arg_count, const FenceCompletionValue completed,
        FenceWaitHook &wait_for_fence) {
        if (arg_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 args_bytes = static_cast<u64>(arg_count) * sizeof(rhi::IndirectDrawArguments);
        if (m_indirect_args[0] != nullptr and m_indirect_args_capacity >= args_bytes) {
            return default_value_v;
        }

        // Growth or first use: same whole-ring drop as the payload ring (and
        // unmap it while a device is still available here). The shared
        // completion array governs both rings, so the youngest-use wait
        // below covers the args ring together with the payload ring.
        {
            const std::span<const FenceCompletionValue> values{m_tile_slot_values.data(), m_tile_slot_values.size()};
            const FenceCompletionValue youngest = maxSlotValue(values);
            if (youngest != kFenceValueNone and youngest > completed) {
                ++m_growth_wait_count;
                PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_fence(youngest));
            }
        }
        releaseIndirectArgs_(std::addressof(device));
        m_tile_slot_values.fill(kFenceValueNone);

        rhi::BufferDesc args_desc{};
        args_desc.size = args_bytes;
        args_desc.elementSize = sizeof(rhi::IndirectDrawArguments);
        args_desc.memoryType = rhi::MemoryType::Upload;
        args_desc.usage = rhi::BufferUsage::IndirectArgument;
        args_desc.defaultState = rhi::ResourceState::IndirectArgument;
        args_desc.label = "grid indirect args";
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (const std::error_code err = make_error_code(
                    device.createBuffer(args_desc, nullptr, m_indirect_args[slot].writeRef()))) [[unlikely]] {
                releaseIndirectArgs_(std::addressof(device));
                return err;
            }
            if (const std::error_code err = make_error_code(device.mapBuffer(
                    m_indirect_args[slot].get(), rhi::CpuAccessMode::Write,
                    std::addressof(m_indirect_args_mapped[slot])))) [[unlikely]] {
                releaseIndirectArgs_(std::addressof(device));
                return err;
            }
        }

        m_indirect_args_capacity = args_bytes;
        return default_value_v;
    }

    void GridPass::releaseIndirectArgs_(rhi::IDevice *const device) noexcept {
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            if (m_indirect_args[slot] == nullptr) {
                m_indirect_args_mapped[slot] = nullptr;
                continue;
            }

            // Same unmap contract as the payload ring: only when a device
            // survived (growth/replacement); shutdown and device loss release
            // still-mapped buffers as-is.
            if (device != nullptr) {
                std::ignore = device->unmapBuffer(m_indirect_args[slot].get());
            }
            m_indirect_args[slot].setNull();
            m_indirect_args_mapped[slot] = nullptr;
        }
        m_indirect_args_capacity = 0u;
    }

    Expected<GridPass::GridCachedPipeline *> GridPass::pipelineFor_(
        rhi::IDevice &device, const RenderPipelineSignature &signature) {
        if (m_shader_program == nullptr) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        // Shape validation runs BEFORE the cache is touched: an unsupported
        // signature must never drop the pipeline built for the last good one.
        // The sample count is a SampleCount by construction — all four
        // enumerators are accepted — so only the color shape is checked.
        if (signature.colorFormats().size() != 1u) {
            PPR_LOG(
                GridPass, error, "unsupported render pipeline signature",
                {
                    {"color_format_count", signature.colorFormats().size()},
                    {"has_depth_stencil", signature.m_depth_stencil_format.has_value()},
                    {"sample_count", static_cast<u32>(signature.m_sample_count)},
                });
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        if (not m_render_pipeline_key.has_value() or
            static_cast<const RenderPipelineSignature &>(m_render_pipeline_key.value()) != signature) {
            PPR_LOG(GridPass, debug, "pipeline cache cleared on signature change");
            // Deferred release, not an immediate drop: in-flight submissions
            // may still reference the old pipeline and its roots. The entry
            // retires once the fence passes the latest submission that could
            // reference it; reclaim runs at every render entry.
            if (m_cached_pipeline.has_value()) {
                GridCachedPipeline retired = std::move(*m_cached_pipeline);
                m_cached_pipeline.reset();
                const FenceCompletionValue last_use = m_last_submitted_value;
                m_deferred_releases.defer(last_use, [held = std::move(retired)]() mutable {
                    held.m_pipeline.setNull();
                    for (rhi::ComPtr<rhi::IShaderObject> &root: held.m_slot_roots) {
                        root.setNull();
                    }
                });
            }
            m_render_pipeline_key.reset();
        }

        if (m_cached_pipeline.has_value()) {
            return &m_cached_pipeline.value();
        }

        rhi::ColorTargetDesc color_target{};
        color_target.format = signature.colorFormats().front();
        color_target.enableBlend = false;

        rhi::RenderPipelineDesc pipeline_desc{};
        pipeline_desc.program = m_shader_program.get();
        pipeline_desc.inputLayout = nullptr;
        pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        pipeline_desc.targets = &color_target;
        pipeline_desc.targetCount = 1u;
        pipeline_desc.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);
        pipeline_desc.depthStencil.format = signature.m_depth_stencil_format.value_or(rhi::Format::Undefined);
        pipeline_desc.depthStencil.depthTestEnable = signature.m_depth_stencil_format.has_value();
        pipeline_desc.depthStencil.depthWriteEnable = signature.m_depth_stencil_format.has_value();
        pipeline_desc.depthStencil.depthFunc = rhi::ComparisonFunc::LessEqual;
        // Tiles are flat quads on the z = 0 plane: culling is off so the winding
        // choice in the vertex entry can never hide a tile.
        pipeline_desc.rasterizer.cullMode = rhi::CullMode::None;
        pipeline_desc.label = "grid tiles pipeline";

        rhi::ComPtr<rhi::IRenderPipeline> pipeline{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, device.createRenderPipeline(pipeline_desc, pipeline.writeRef()));

        // ONE persistent ROOT shader object PER RING SLOT, each built from
        // the same program as the pipeline: the 2-arg bindPipeline overload
        // downcasts its argument to a RootShaderObject, so
        // createShaderObject(type, container) — which returns a plain
        // ShaderObject — cannot back it. A frame binds and writes only its
        // selected slot's root, so per-frame constants never mutate an
        // object an in-flight submission still references.
        std::array<rhi::ComPtr<rhi::IShaderObject>, kTileRingSize> slot_roots{};
        std::array<rhi::ShaderCursor, kTileRingSize> slot_frame_cursors{};
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            rhi::ComPtr<rhi::IShaderObject> root_object{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, device.createRootShaderObject(pipeline->getProgram(), root_object.writeRef()));

            const rhi::ShaderCursor invariant_cursor{root_object.get()};
            const rhi::ShaderCursor frame_field = invariant_cursor["g_grid_frame"];
            if (not frame_field.isValid()) [[unlikely]] {
                // A missing field means the pass and the shader disagree; fail
                // closed rather than write through a null cursor.
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            // Dereferenced ONCE here: the per-frame path then only writes the
            // GridFrame through the cached cursor.
            rhi::ShaderCursor frame_cursor{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, frame_field.getDereferenced(frame_cursor));

            slot_frame_cursors[slot] = frame_cursor;
            slot_roots[slot] = std::move(root_object);
        }

        m_render_pipeline_key.emplace(signature);
        m_cached_pipeline.emplace(
            GridCachedPipeline{
                .m_pipeline = std::move(pipeline),
                .m_slot_roots = std::move(slot_roots),
                .m_slot_frame_cursors = std::move(slot_frame_cursors),
            });

        return &m_cached_pipeline.value();
    }

    // ------------------------------------------------------------------
    // teardown and resource initialization
    // ------------------------------------------------------------------

    std::error_code GridPass::shutdown() {
        PPR_LOG(
            GridPass, info, "GridPass shut down",
            {
                {"staged_tiles", m_submitted_tiles.size()},
                {"cached_tiles", m_tile_cache.size()},
            });

        // Teardown order: stop submissions first (no new work), then drop the
        // cached pipeline roots (their bindings reference the payload ring),
        // then the CPU tile cache, then the pass's own program and buffers.
        // Precondition (established by the Editor): GPU completion was
        // already proven idle, so no pending frame can read what is
        // released here. Deferred releases drain unconditionally — after
        // confirmed loss the objects are dead either way. Slot values reset
        // alongside the fence epoch they named. Retain-first-error,
        // best-effort.
        std::error_code first_err{};
        m_submitted_tiles.clear();
        clearCache();
        m_upload_count = 0u;
        m_cache_hit_count = 0u;
        m_atlas_serialization_count = 0u;
        m_atlas_write_count = 0u;
        m_cell_size_m = 1.0f;
        m_cell_slot_values.fill(kFenceValueNone);
        m_resident_cell_slots.reset();
        m_atlas_deferred_count = 0u;
        m_atlas_uploaded_bytes = 0u;
        m_resident_table.clear();
        m_resident_table_build_count = 0u;
        m_resident_table_upload_count = 0u;
        m_growth_wait_count = 0u;
        std::ignore = m_deferred_releases.drainAll();
        m_last_submitted_value = kFenceValueNone;
        m_tile_slot_values.fill(kFenceValueNone);
        m_last_used_slot = 0u;
        m_cached_pipeline.reset();
        m_render_pipeline_key.reset();
        m_ready = false;

        m_shader_program.setNull();
        m_visibility_programs = {};
        m_visibility_slots = {};
        m_visibility_prepared = false;
        m_cell_atlas.setNull();
        m_cell_atlas_mapped = nullptr;
        m_cell_dummy.setNull();
        releaseIndirectArgs_(nullptr);
        releaseTilePayloads_(nullptr);
        return first_err;
    }

    std::error_code GridPass::notifyDeviceLost() noexcept {
        // Same order as shutdown, retain-first-error. CPU records (staged
        // submissions, tile cache, counters) are KEPT: resolveCached still
        // serves quiescent tiles while GPU-touching calls park. Programs
        // survive device loss, cached pipelines do not. Deferred releases
        // are discarded outright — the GPU objects are gone, so nothing is
        // in flight worth waiting for (same contract as the Renderer and the
        // pass-owned caches). Slot values reset with the fence epoch.
        std::error_code first_err{};
        m_submitted_tiles.clear();
        m_resident_table_stale = true;
        cancelCellCohort_();
        m_cell_slots.fill(std::nullopt);
        m_cell_slots_current.reset();
        m_resident_cell_slots.reset();
        m_cell_slot_values.fill(kFenceValueNone);
        m_cell_publication_deferred = false;
        std::ignore = m_deferred_releases.drainAll();
        m_last_submitted_value = kFenceValueNone;
        m_tile_slot_values.fill(kFenceValueNone);
        m_last_used_slot = 0u;
        m_cached_pipeline.reset();
        m_render_pipeline_key.reset();
        m_cell_atlas.setNull();
        m_visibility_programs = {};
        m_visibility_slots = {};
        m_visibility_prepared = false;
        m_cell_atlas_mapped = nullptr;
        m_cell_dummy.setNull();
        for (auto it = m_cell_cache.begin(); it != m_cell_cache.end(); ++it) {
            it->second.m_pending = true;
        }
        releaseIndirectArgs_(nullptr);
        releaseTilePayloads_(nullptr);
        if (not first_err) {
            m_ready = false;
        }
        return first_err;
    }

    std::error_code GridPass::createShaderProgram_(
        IShaderService &shader_service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule grid_shader{};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            shader_service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("grid_tiles.slang"), "grid_tiles", grid_shader.writeRef()));

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, grid_shader->findEntryPointByName("vertexGridMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, grid_shader->findEntryPointByName("fragmentGridMain", fragment_ep.writeRef()));

        // One program: the tile-indexed vertex entry reads
        // g_tiles[g_tile_base + SV_InstanceID]. The draw's tile base arrives
        // as that entry's `uniform uint g_tile_base` parameter, bound per draw
        // via shader_cursor["g_tile_base"].setData(...), and never through
        // the draw arguments.
        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = grid_shader.get();
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

        return default_value_v;
    }

    std::error_code GridPass::createVisibilityPrograms_(
        IShaderService &service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("grid_visibility.slang"), "grid_visibility", module.writeRef()));
        std::array<VisibilityProgram, 2u> programs{};
        const std::array<const char *, 2u> names{"resetVisibility", "cullVisibility"};
        for (u32 index = 0u; index < programs.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(names[index], entry.writeRef()));
            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module.get();
            desc.slangEntryPoints = entries;
            desc.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            VisibilityProgram &program = programs[index];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createShaderProgram(desc, program.m_program.writeRef(), diagnostics.writeRef()));
            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = program.m_program.get();
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createComputePipeline(pipeline, program.m_pipeline.writeRef()));
            for (auto &root: program.m_roots) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, device.createRootShaderObject(program.m_program.get(), root.writeRef()));
            }
        }
        m_visibility_programs = std::move(programs);
        return {};
    }

    std::error_code GridPass::ensureVisibilityBuffers_(rhi::IDevice &device) {
        if (m_visibility_slots[0].m_ids) {
            return {};
        }
        std::array<VisibilitySlot, kTileRingSize> slots{};
        for (VisibilitySlot &slot: slots) {
            rhi::BufferDesc ids{};
            ids.size = kGridTileCacheCapacity * sizeof(u32);
            ids.elementSize = sizeof(u32);
            ids.memoryType = rhi::MemoryType::DeviceLocal;
            ids.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            ids.defaultState = rhi::ResourceState::UnorderedAccess;
            ids.label = "grid GPU visible IDs";
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(ids, nullptr, slot.m_ids.writeRef()));
            rhi::BufferDesc args = ids;
            args.size = sizeof(rhi::IndirectDrawArguments);
            args.elementSize = sizeof(rhi::IndirectDrawArguments);
            args.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::IndirectArgument) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            args.label = "grid GPU draw arguments";
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(args, nullptr, slot.m_args.writeRef()));
        }
        m_visibility_slots = std::move(slots);
        return {};
    }

    std::error_code GridPass::rebuildResidentTable_() {
        Array<GridTilePayload> payloads;
        payloads.reserve(m_submitted_tiles.size());
        bool has_cells = false;
        for (const GridTileSubmission &tile: m_submitted_tiles) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSubmission_(tile));
            const auto cells = m_cell_cache.find(tile.m_chunk_id);
            if (cells != m_cell_cache.end()) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, not cells->second.m_pending and m_cell_atlas != nullptr
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::operation_in_progress));
                has_cells = true;
            }
            GridTilePayload payload{};
            const auto cached = resolveCached(tile.m_chunk_id);
            if (cached) {
                payload = *cached;
            } else {
                payload.m_material = tile.m_material_id;
            }
            // Submission bounds are authoritative for both the reference cull
            // and GPU geometry, including clean submissions with cached data.
            applySubmissionBounds_(payload, tile.m_tile_range);
            applyCellPayload_(payload, tile.m_chunk_id);
            payloads.push_back(payload);
        }
        m_resident_cell_slots.reset();
        for (const auto &payload: payloads) {
            if ((payload.m_flags & 1u) != 0u) {
                m_resident_cell_slots.set(payload.m_cell_word_base / kCellWordsPerChunk);
            }
        }
        m_resident_table = std::move(payloads);
        m_visibility_has_cells = has_cells;
        m_resident_table_stale = false;
        m_resident_table_uploaded.fill(false);
        ++m_resident_table_build_count;
        return {};
    }

    std::error_code GridPass::prepareGpuVisibility(const PreRenderContext &context) {
        m_visibility_prepared = false;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_submitted_tiles.size() <= kGridTileCacheCapacity
                          ? std::error_code{}
                          : std::make_error_code(std::errc::no_buffer_space));
        reclaimCompleted_(context.m_completed_fence_value);
        if (m_resident_table_stale and not m_cell_publication_deferred) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, rebuildResidentTable_());
        }
        const auto &payloads = m_resident_table;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(context.m_device, false));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureVisibilityBuffers_(context.m_device));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, ensureTilePayloads_(
                          context.m_device, std::max<u32>(1u, safe_narrowing(payloads.size())),
                          context.m_completed_fence_value, context.m_wait_for_fence));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, selectSlot_(context.m_completed_fence_value, context.m_wait_for_fence));
        if (not payloads.empty() and not m_resident_table_uploaded[m_tile_ring_cursor]) {
            std::memcpy(
                m_tile_payload_mapped[m_tile_ring_cursor], payloads.data(), payloads.size() * sizeof(GridTilePayload));
            m_resident_table_uploaded[m_tile_ring_cursor] = true;
            ++m_resident_table_upload_count;
        }

        struct alignas(16) VisibilityFrame {
            float m_planes[6][4]{};
            u32 m_count{};
            float m_cell_size_m{1.0f};
            std::array<u32, 2u> m_padding{};
        };
        static_assert(sizeof(VisibilityFrame) == 112u);
        VisibilityFrame frame{};
        for (u32 plane = 0u; plane < 6u; ++plane) {
            std::memcpy(
                frame.m_planes[plane], m_camera_view.m_frustum.clip[plane].data(), sizeof(frame.m_planes[plane]));
        }
        frame.m_count = safe_narrowing(payloads.size());
        frame.m_cell_size_m = m_cell_size_m;
        VisibilitySlot &slot = m_visibility_slots[m_tile_ring_cursor];
        context.m_encoder.setBufferState(slot.m_args, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setBufferState(slot.m_ids, rhi::ResourceState::UnorderedAccess);
        for (u32 phase = 0u; phase < 2u; ++phase) {
            VisibilityProgram &program = m_visibility_programs[phase];
            rhi::IShaderObject *const root = program.m_roots[m_tile_ring_cursor].get();
            rhi::ShaderCursor cursor{root};
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_args"].setBinding(rhi::Binding(slot.m_args, makeFullRange(slot.m_args))));
            if (phase == 1u) {
                rhi::ShaderCursor constants{};
                PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_visibility"].getDereferenced(constants));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&frame, sizeof(frame)));
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor["g_visible_ids"].setBinding(rhi::Binding(slot.m_ids, makeFullRange(slot.m_ids))));
                rhi::IBuffer *const tiles = m_tile_payloads[m_tile_ring_cursor].get();
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor["g_tiles"].setBinding(rhi::Binding(tiles, makeFullRange(tiles))));
            }
            rhi::IComputePassEncoder *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(program.m_pipeline, root);
                pass->dispatchCompute(phase == 0u ? 1u : std::max(1u, (frame.m_count + 63u) / 64u), 1u, 1u);
            }
            context.m_encoder.globalBarrier();
        }
        context.m_encoder.setBufferState(slot.m_args, rhi::ResourceState::IndirectArgument);
        context.m_encoder.setBufferState(slot.m_ids, rhi::ResourceState::ShaderResource);
        m_visibility_prepared = true;
        return {};
    }

    std::error_code GridPass::renderGpuIndirect(const DrawContext &context) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            m_visibility_prepared ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));
        m_visibility_prepared = false;
        const auto cached = pipelineFor_(context.m_device, context.m_render_pipeline_key);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cached ? std::error_code{} : cached.error());
        rhi::IShaderObject *const root = (*cached)->m_slot_roots[m_tile_ring_cursor].get();
        rhi::ShaderCursor cursor{root};
        GridFrame frame{};
        frame.m_view_projection = m_camera_view.m_view_projection;
        frame.m_use_visible_ids = 1u;
        frame.m_cell_size_m = m_cell_size_m;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, (*cached)->m_slot_frame_cursors[m_tile_ring_cursor].setData(&frame, sizeof(frame)));
        const u32 base = 0u;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_tile_base"].setData(&base, sizeof(base)));
        rhi::IBuffer *const tiles = m_tile_payloads[m_tile_ring_cursor].get();
        VisibilitySlot &slot = m_visibility_slots[m_tile_ring_cursor];
        rhi::IBuffer *const cells = m_visibility_has_cells ? m_cell_atlas.get() : m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_tiles"].setBinding(rhi::Binding(tiles, makeFullRange(tiles))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_visible_ids"].setBinding(rhi::Binding(slot.m_ids, makeFullRange(slot.m_ids))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_cell_materials"].setBinding(rhi::Binding(cells, makeFullRange(cells))));
        context.m_pass.bindPipeline((*cached)->m_pipeline, root);
        context.m_pass.drawIndirect(1u, rhi::BufferOffsetPair{slot.m_args});
        return {};
    }

    std::error_code GridPass::uploadFrameConstants_(rhi::ShaderCursor &frame_cursor) {
        GridFrame frame{};
        frame.m_view_projection = m_camera_view.m_view_projection;
        frame.m_cell_size_m = m_cell_size_m;

        PPR_RETURN_ERROR_ON_FAIL(GridPass, frame_cursor.setData(&frame, sizeof(GridFrame)));

        return default_value_v;
    }
}
