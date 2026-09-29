module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.grid_pass;
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
        [[nodiscard]] Box tileBox_(const GridTileRange &range) noexcept {
            const float center_x = (static_cast<float>(range.m_min_x) + static_cast<float>(range.m_max_x)) * 0.5f;
            const float center_y = (static_cast<float>(range.m_min_y) + static_cast<float>(range.m_max_y)) * 0.5f;
            const float half_x = (static_cast<float>(range.m_max_x) - static_cast<float>(range.m_min_x)) * 0.5f;
            const float half_y = (static_cast<float>(range.m_max_y) - static_cast<float>(range.m_min_y)) * 0.5f;
            const float half = std::max({half_x, half_y, 0.5f}) + GridPass::kGridCullEpsilon;
            return Box{float3{center_x, center_y, 0.0f}, half * 2.0f};
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
        if (range.m_min_x < 0 or range.m_min_y < 0 or range.m_max_x > kGridWorldEdge or range.m_max_y > kGridWorldEdge) [[unlikely]] {
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
        const CameraSnapshot &snapshot,
        const std::span<const GridTileSubmission> submissions) {
        GridTilePlan plan{};
        for (std::size_t index = 0u; index < submissions.size(); ++index) {
            const GridTileSubmission &submission = submissions[index];
            if (const std::error_code err = validateSubmission_(submission)) [[unlikely]] {
                return std::unexpected{err};
            }
            if (not snapshot.m_frustum.isVisible(tileBox_(submission.m_tile_range))) {
                continue;
            }
            plan.m_tiles.push_back(GridPlannedTile{
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
    Expected<Array<rhi::IndirectDrawArguments> > GridPass::compactIndirect(const GridTilePlan &plan) {
        if (plan.m_tiles.size() > kGridTileCacheCapacity) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }

        Array<rhi::IndirectDrawArguments> args{};
        args.reserve(plan.m_tiles.size());
        for (std::size_t index = 0u; index < plan.m_tiles.size(); ++index) {
            args.push_back(rhi::IndirectDrawArguments{
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

    std::error_code GridPass::initialize(IRhiService &rhi_service, IShaderService &shader_service, const fs::path &content_dir) {
        rhi::IDevice &device = rhi_service.getDevice();

        bool shader_state_started = false;
        bool initialization_complete = false;
        PPR_DEFER{
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

        m_ready = true;
        initialization_complete = true;

        PPR_LOG(GridPass, info, "GridPass initialized");
        return default_value_v;
    }

    std::error_code GridPass::update([[maybe_unused]] TimeSpan dt, const CameraSnapshot &camera_view) {
        m_camera_view = camera_view;

        return default_value_v;
    }

    std::error_code GridPass::submitTiles(const std::span<const GridTileSubmission> submissions) {
        // Pure CPU: staging needs no device, so device-free tests and the
        // colony translator can stage before (or without) initialize; only
        // render touches the GPU.
        for (const GridTileSubmission &submission: submissions) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSubmission_(submission));
        }
        m_submitted_tiles.clear();
        m_submitted_tiles.reserve(submissions.size());
        for (const GridTileSubmission &submission: submissions) {
            m_submitted_tiles.push_back(submission);
        }
        return default_value_v;
    }

    void GridPass::clearTiles() noexcept {
        m_submitted_tiles.clear();
    }

    void GridPass::clearCache() noexcept {
        m_tile_cache.clear();
        m_cell_cache.clear();
        // Keep the atlas allocated: its previous contents may still be read
        // by an in-flight draw. A future cell request repopulates its slot
        // only after prepareCellUploads fences the queue.
    }

    Expected<u32> GridPass::requestUpload(const GridUploadRequest &request) {
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
        if (is_new_entry and m_tile_cache.size() >= kGridTileCacheCapacity)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }

        // Pack before touching either cache. The low halfword is even cell i,
        // the high halfword is cell i+1 (including transparent 0xffff).
        std::unique_ptr<std::array<u32, kCellWordsPerChunk> > packed{};
        if (has_cells) {
            packed = std::make_unique<std::array<u32, kCellWordsPerChunk> >();
            for (u32 word = 0u; word < kCellWordsPerChunk; ++word) {
                (*packed)[word] = static_cast<u32>(request.m_cell_materials[word * 2u])
                                  | (static_cast<u32>(request.m_cell_materials[word * 2u + 1u]) << 16u);
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
        if (has_cells) {
            m_cell_cache.insert_or_assign(request.m_chunk_id, GridCachedCells{std::move(packed), true});
        } else {
            m_cell_cache.erase(request.m_chunk_id);
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
            if (const std::error_code err = make_error_code(device.mapBuffer(
                buffer.get(), rhi::CpuAccessMode::Write, &m_cell_atlas_mapped))) [[unlikely]] {
                buffer.setNull();
                return err;
            }
        }
        return default_value_v;
    }

    std::error_code GridPass::prepareCellUploads(rhi::IDevice &device,
                                                 std23::function_ref<std::error_code()> wait_for_gpu_idle) {
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        const Expected<GridTilePlan> plan = planTiles(m_camera_view,
            std::span<const GridTileSubmission>{m_submitted_tiles.data(), m_submitted_tiles.size()});
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }

        Array<u32> pending{};
        for (const GridPlannedTile &tile: plan->m_tiles) {
            const auto found = m_cell_cache.find(tile.m_chunk_id);
            if (found != m_cell_cache.end() and found->second.m_pending and
                std::find(pending.begin(), pending.end(), tile.m_chunk_id) == pending.end()) {
                pending.push_back(tile.m_chunk_id);
            }
        }
        if (pending.empty()) {
            return default_value_v;
        }

        // This is a renderer-owned queue fence, not a pass submission. Never
        // write a mapped Upload SRV while an earlier frame may still read it.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, wait_for_gpu_idle());
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(device, true));
        if (m_cell_atlas_mapped == nullptr) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        auto *const atlas = static_cast<u32 *>(m_cell_atlas_mapped);
        for (const u32 chunk_id: pending) {
            GridCachedCells &cells = m_cell_cache.find(chunk_id)->second;
            std::memcpy(atlas + static_cast<std::size_t>(chunk_id) * kCellWordsPerChunk,
                cells.m_words->data(), sizeof(u32) * kCellWordsPerChunk);
            cells.m_pending = false;
        }
        return default_value_v;
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
            payload.m_cell_word_base = chunk_id * kCellWordsPerChunk;
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
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (m_submitted_tiles.empty()) {
            return default_value_v;
        }

        Expected<GridTilePlan> plan = planTiles(m_camera_view,
            std::span<const GridTileSubmission>{m_submitted_tiles.data(), m_submitted_tiles.size()});
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

        // Resolve payloads: cache hits serve quiescent tiles, while a tile
        // with no cache entry yet stages cold straight from its submission so
        // the CPU-staged path still draws before its first upload.
        Array<GridTilePayload> payloads{};
        payloads.reserve(plan->m_tiles.size());
        for (const GridPlannedTile &tile: plan->m_tiles) {
            Expected<GridTilePayload> cached = resolveCached(tile.m_chunk_id);
            if (cached.has_value()) {
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

        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            ensureTilePayloads_(draw_context.m_device, safe_narrowing(payloads.size())));

        // One rotation per submit: the draw encoded after this point reads the
        // slot just advanced onto, which the previous submit did not write.
        // The slot stays mapped for its whole life, so there is no per-frame
        // map/unmap pair. The rotation is per render() submit and the 3-slot
        // parity assumes a SINGLE GridPass::render per frame (backpressured by
        // acquireNextImage).
        m_tile_ring_cursor = (m_tile_ring_cursor + 1u) % kTileRingSize;
        void *const mapped_payloads = m_tile_payload_mapped[m_tile_ring_cursor];
        if (mapped_payloads == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        std::memcpy(mapped_payloads, payloads.data(), static_cast<std::size_t>(payloads.size()) * sizeof(GridTilePayload));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .indexBuffer = {},
        });

        rhi::IBuffer *const payload_buffer = m_tile_payloads[m_tile_ring_cursor].get();
        if (payload_buffer == nullptr or payload_buffer->getDesc().elementSize != sizeof(GridTilePayload)) [[unlikely]] {
            PPR_LOG(GridPass, error, "tile payload stride mismatch vs shader expectation", {
                {"payload_stride", payload_buffer == nullptr ? 0u : payload_buffer->getDesc().elementSize},
            });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<GridCachedPipeline *> cached = pipelineFor_(
            draw_context.m_device,
            draw_context.m_render_pipeline_key);
        if (not cached.has_value()) [[unlikely]] {
            return cached.error();
        }
        rhi::IRenderPipeline *const pipeline = (*cached)->m_pipeline.get();
        rhi::IShaderObject *const root_object = (*cached)->m_root_object.get();
        if (pipeline == nullptr or root_object == nullptr or not (*cached)->m_frame_cursor.isValid()) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // Persistent root object: rebound as-is; g_grid_frame's cursor was
        // fixed at creation, so only the data below is written per frame.
        draw_context.m_pass.bindPipeline(pipeline, root_object);
        rhi::ShaderCursor shader_cursor{root_object};

        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_((*cached)->m_frame_cursor));

        // SV_InstanceID is draw-local on D3D12, SPIR-V, and Metal alike, so the
        // startInstanceLocation never reaches the shader. The tile base rides
        // the vertex entry's uniform param instead and the draw starts at
        // instance 0; binding both would double-count the offset.
        const u32 tile_base = 0u;
        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            shader_cursor["g_tile_base"].setData(&tile_base, sizeof(tile_base)));

        PPR_RETURN_ERROR_ON_FAIL(GridPass, shader_cursor["g_tiles"].setBinding(
            rhi::Binding(payload_buffer, makeFullRange(payload_buffer))));
        rhi::IBuffer *const cell_buffer = has_cells ? m_cell_atlas.get() : m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(GridPass, shader_cursor["g_cell_materials"].setBinding(
            rhi::Binding(cell_buffer, makeFullRange(cell_buffer))));

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
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (m_submitted_tiles.empty()) {
            return default_value_v;
        }

        Expected<GridTilePlan> plan = planTiles(m_camera_view,
            std::span<const GridTileSubmission>{m_submitted_tiles.data(), m_submitted_tiles.size()});
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

        Expected<Array<rhi::IndirectDrawArguments> > args = compactIndirect(*plan);
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

        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            ensureTilePayloads_(draw_context.m_device, safe_narrowing(payloads.size())));
        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            ensureIndirectArgs_(draw_context.m_device, safe_narrowing(args->size())));

        // One rotation per submit, shared with the payload ring: the draws
        // encoded after this point read the slot just advanced onto, which
        // the previous submit did not write. Both slots stay mapped for their
        // whole life, so there is no per-frame map/unmap pair.
        m_tile_ring_cursor = (m_tile_ring_cursor + 1u) % kTileRingSize;
        void *const mapped_payloads = m_tile_payload_mapped[m_tile_ring_cursor];
        void *const mapped_args = m_indirect_args_mapped[m_tile_ring_cursor];
        if (mapped_payloads == nullptr or mapped_args == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        std::memcpy(mapped_payloads, payloads.data(), static_cast<std::size_t>(payloads.size()) * sizeof(GridTilePayload));
        std::memcpy(mapped_args, args->data(), static_cast<std::size_t>(args->size()) * sizeof(rhi::IndirectDrawArguments));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .indexBuffer = {},
        });

        rhi::IBuffer *const payload_buffer = m_tile_payloads[m_tile_ring_cursor].get();
        if (payload_buffer == nullptr or payload_buffer->getDesc().elementSize != sizeof(GridTilePayload)) [[unlikely]] {
            PPR_LOG(GridPass, error, "tile payload stride mismatch vs shader expectation", {
                {"payload_stride", payload_buffer == nullptr ? 0u : payload_buffer->getDesc().elementSize},
            });
            return make_error_code(std::errc::invalid_argument);
        }

        rhi::IBuffer *const args_buffer = m_indirect_args[m_tile_ring_cursor].get();
        if (args_buffer == nullptr or args_buffer->getDesc().elementSize != sizeof(rhi::IndirectDrawArguments)) [[unlikely]] {
            PPR_LOG(GridPass, error, "indirect args stride mismatch vs drawIndirect expectation", {
                {"args_stride", args_buffer == nullptr ? 0u : args_buffer->getDesc().elementSize},
            });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<GridCachedPipeline *> cached = pipelineFor_(
            draw_context.m_device,
            draw_context.m_render_pipeline_key);
        if (not cached.has_value()) [[unlikely]] {
            return cached.error();
        }
        rhi::IRenderPipeline *const pipeline = (*cached)->m_pipeline.get();
        rhi::IShaderObject *const root_object = (*cached)->m_root_object.get();
        if (pipeline == nullptr or root_object == nullptr or not (*cached)->m_frame_cursor.isValid()) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // Same bindings as the CPU path: the persistent root object rebound
        // as-is, the frame constants, g_tile_base 0 (per-draw bases ride the
        // args, see above), and the payload buffer.
        draw_context.m_pass.bindPipeline(pipeline, root_object);
        rhi::ShaderCursor shader_cursor{root_object};

        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_((*cached)->m_frame_cursor));

        const u32 tile_base = 0u;
        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            shader_cursor["g_tile_base"].setData(&tile_base, sizeof(tile_base)));

        PPR_RETURN_ERROR_ON_FAIL(GridPass, shader_cursor["g_tiles"].setBinding(
            rhi::Binding(payload_buffer, makeFullRange(payload_buffer))));
        rhi::IBuffer *const cell_buffer = has_cells ? m_cell_atlas.get() : m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(GridPass, shader_cursor["g_cell_materials"].setBinding(
            rhi::Binding(cell_buffer, makeFullRange(cell_buffer))));

        draw_context.m_pass.drawIndirect(
            safe_narrowing(args->size()),
            rhi::BufferOffsetPair{args_buffer});

        return default_value_v;
    }

    std::error_code GridPass::ensureTilePayloads_(rhi::IDevice &device, const u32 tile_count) {
        if (tile_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 payload_bytes = static_cast<u64>(tile_count) * sizeof(GridTilePayload);
        if (m_tile_payloads[0] != nullptr and m_tile_payload_capacity >= payload_bytes) {
            return default_value_v;
        }

        // Growth or first use: drop the whole ring (and unmap it, since a
        // device is still available here) before rebuilding it at the new size.
        releaseTilePayloads_(std::addressof(device));

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

    std::error_code GridPass::ensureIndirectArgs_(rhi::IDevice &device, const u32 arg_count) {
        if (arg_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 args_bytes = static_cast<u64>(arg_count) * sizeof(rhi::IndirectDrawArguments);
        if (m_indirect_args[0] != nullptr and m_indirect_args_capacity >= args_bytes) {
            return default_value_v;
        }

        // Growth or first use: same whole-ring drop as the payload ring (and
        // unmap it while a device is still available here).
        releaseIndirectArgs_(std::addressof(device));

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
        rhi::IDevice &device,
        const RenderPipelineSignature &signature) {
        if (m_shader_program == nullptr) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        // Shape validation runs BEFORE the cache is touched: an unsupported
        // signature must never drop the pipeline built for the last good one.
        // The sample count is a SampleCount by construction — all four
        // enumerators are accepted — so only the color shape is checked.
        if (signature.colorFormats().size() != 1u) {
            PPR_LOG(GridPass, error, "unsupported render pipeline signature", {
                {"color_format_count", signature.colorFormats().size()},
                {"has_depth_stencil", signature.m_depth_stencil_format.has_value()},
                {"sample_count", static_cast<u32>(signature.m_sample_count)},
            });
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        if (not m_render_pipeline_key.has_value() or
            static_cast<const RenderPipelineSignature &>(m_render_pipeline_key.value()) != signature) {
            PPR_LOG(GridPass, debug, "pipeline cache cleared on signature change");
            m_cached_pipeline.reset();
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

        // ONE persistent ROOT shader object, built from the same program as
        // the pipeline: the 2-arg bindPipeline overload downcasts its argument
        // to a RootShaderObject, so createShaderObject(type, container) —
        // which returns a plain ShaderObject — cannot back it.
        rhi::ComPtr<rhi::IShaderObject> root_object{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass,
            device.createRootShaderObject(pipeline->getProgram(), root_object.writeRef()));

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

        m_render_pipeline_key.emplace(signature);
        m_cached_pipeline.emplace(GridCachedPipeline{
            .m_pipeline = std::move(pipeline),
            .m_root_object = std::move(root_object),
            .m_frame_cursor = frame_cursor,
        });

        return &m_cached_pipeline.value();
    }

    // ------------------------------------------------------------------
    // teardown and resource initialization
    // ------------------------------------------------------------------

    std::error_code GridPass::shutdown() {
        PPR_LOG(GridPass, info, "GridPass shut down", {
            {"staged_tiles", m_submitted_tiles.size()},
            {"cached_tiles", m_tile_cache.size()},
        });

        // Teardown order: stop submissions first (no new work), then drop the
        // cached pipeline root (its bindings reference the payload ring),
        // then the CPU tile cache, then the pass's own program and buffers.
        // Retain-first-error, best-effort, and all of it BEFORE the
        // renderer's waitOnHost.
        std::error_code first_err{};
        m_submitted_tiles.clear();
        clearCache();
        m_upload_count = 0u;
        m_cache_hit_count = 0u;
        m_cached_pipeline.reset();
        m_render_pipeline_key.reset();
        m_ready = false;

        m_shader_program.setNull();
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
        // survive device loss, cached pipelines do not.
        std::error_code first_err{};
        m_submitted_tiles.clear();
        m_cached_pipeline.reset();
        m_render_pipeline_key.reset();
        m_cell_atlas.setNull();
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

    std::error_code GridPass::createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule grid_shader{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, shader_service.loadModuleFromFile(
            content_dir / TEXT("shaders") / TEXT("grid_tiles.slang"),
            "grid_tiles",
            grid_shader.writeRef()));

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            grid_shader->findEntryPointByName("vertexGridMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            grid_shader->findEntryPointByName("fragmentGridMain", fragment_ep.writeRef()));

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
        PPR_RETURN_ERROR_ON_FAIL(GridPass,
            device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

        return default_value_v;
    }

    std::error_code GridPass::uploadFrameConstants_(rhi::ShaderCursor &frame_cursor) {
        GridFrame frame{};
        frame.m_view_projection = m_camera_view.m_view_projection;

        PPR_RETURN_ERROR_ON_FAIL(GridPass, frame_cursor.setData(&frame, sizeof(GridFrame)));

        return default_value_v;
    }
}
