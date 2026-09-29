module;
#include "pP/Macros.h"
export module engine.app:renderer.grid_pass;

import :renderer.types;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // grid pass: chunk-sized tile quads over the cell world
    // ------------------------------------------------------------------
    // Content-specific pass owning its shader/pipeline/buffers/GPU tile
    // cache; Renderer stays content-free. Consumes CameraSnapshot (never a
    // mutable Camera while drawing); viewport and scissor travel per-draw on
    // DrawSubmission; row-major row-vector LH +Z-forward +Y-up, [0,1] depth,
    // mul(float4,matrix), view*projection, no backend transpose/Y-flip.
    // NO GI inputs of any kind.
    //
    // Tiles reuse the single 32x32-chunk x 128x128-cell geometry as
    // CHUNK-SIZED tiles (kTilesPerChunk == 1): one tile per chunk, never a
    // second tiling system. Finer tiling is unjustified: 1024 frustum tests
    // per frame are trivially cheap next to a second tile hierarchy.
    // Frustum/visibility runs per tile from the snapshot bounds; quiescent
    // (clean) and off-screen tiles are skipped by the plan.
    //
    // Graph note: game/colony (which owns the engine.sim link) translates
    // grid state into these plain structs; engine.app gains NO engine.sim
    // dependency and no sim type crosses this boundary.

    // Tile rect in world-cell units. Signed so camera-centred test layouts
    // stay representable; production submissions always land in [0, edge).
    struct GridTileRange {
        i32 m_min_x = 0;
        i32 m_min_y = 0;
        i32 m_max_x = 0;
        i32 m_max_y = 0;
    };

    // Frozen boundary struct (plain data): one chunk-sized tile submission.
    // m_chunk_id is the row-major chunk index (the sim chunkIndexOf domain,
    // carried here as a plain u32). m_dirty_mask bit i marks tile i dirty;
    // chunk-sized tiles use bit 0 only and every higher bit is rejected.
    // m_material_id is the element/material tint index.
    struct GridTileSubmission {
        u32 m_chunk_id = 0u;
        GridTileRange m_tile_range{};
        u64 m_dirty_mask = 0u;
        u32 m_material_id = 0u;
    };

    // Tile-quad GPU payload mirror. m_rect is min_x, min_y, max_x, max_y
    // in world-cell units on the z = 0 plane. Bit 0 selects the cell atlas;
    // otherwise the original flat m_material tint is unchanged.
    struct alignas(16) GridTilePayload {
        float m_rect[4]{};
        u32 m_material = 0u;
        u32 m_cell_word_base = 0u;
        u32 m_flags = 0u;
        u32 m_pad = 0u;
    };

    static_assert(std::is_trivially_copyable_v<GridTilePayload>);
    static_assert(std::is_standard_layout_v<GridTilePayload>);
    static_assert(sizeof(GridTilePayload) == 32u);
    static_assert(alignof(GridTilePayload) == 16u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_rect) == 0u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_material) == 16u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_cell_word_base) == 20u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_flags) == 24u);
    static_assert(PPR_OFFSETOF(GridTilePayload, m_pad) == 28u);

    // Frozen boundary struct (plain data): dirty-chunk tile upload. The view
    // is read-only and carries one payload per dirty tile in tile-index
    // order; only dirty tiles are uploaded, quiescent tiles are served from
    // the pass-owned cache.
    struct GridUploadRequest {
        u32 m_chunk_id = 0u;
        std::span<const GridTilePayload> m_tile_data_view{};
        // Optional, borrowed row-major 128x128 u16 material IDs. Empty means
        // flat tile; 0xffff denotes transparent. Copied before return.
        std::span<const u16> m_cell_materials{};
    };

    // One visible tile: the submission index it planned from plus the
    // resolved draw inputs. The plan preserves submission order.
    struct GridPlannedTile {
        u32 m_submission_index = 0u;
        u32 m_chunk_id = 0u;
        GridTileRange m_tile_range{};
        u32 m_material_id = 0u;
    };

    struct GridTilePlan {
        Array<GridPlannedTile> m_tiles{};
    };

    class GridPass {
    public:
        // Chunk-sized tiles: one tile per chunk over the 32x32-chunk grid of
        // 128x128-cell chunks (plain values here; no sim include).
        static constexpr u32 kTilesPerChunk{1u};
        static constexpr u32 kGridTileEdge{128u};
        static constexpr i32 kGridWorldEdge{4096};

        // Boundary rule: tile boxes are expanded by this epsilon (in
        // world-cell units) before the frustum test, so edge-touching tiles
        // are VISIBLE by construction while strictly-outside tiles stay
        // culled regardless of the frustum plane-test inclusivity.
        static constexpr float kGridCullEpsilon{0.5f};

        // Upload-cache capacity, clamped per bindless policy: one entry per
        // chunk of the whole world, bump-allocated with deterministic
        // fail-closed (no_buffer_space) overflow, never partial upload.
        static constexpr u32 kGridTileCacheCapacity{1024u};
        static constexpr u32 kCellsPerChunk{kGridTileEdge * kGridTileEdge};
        static constexpr u32 kCellWordsPerChunk{kCellsPerChunk / 2u};
        static constexpr u32 kCellAtlasWords{kGridTileCacheCapacity * kCellWordsPerChunk};

        // Indirect-lane vertex count: 6 non-indexed verts per tile quad, the
        // same geometry the CPU-staged drawInstanced path draws.
        static constexpr u32 kGridTileVertexCount{6u};

        // Per-frame constants: the unjittered view-projection only. Jitter
        // must NEVER enter the pipeline key or the frame upload.
        struct GridFrame {
            float4x4 m_view_projection{identity_v};
        };

        static_assert(sizeof(GridFrame) == 64u);

        // Pure CPU planner: keeps the submissions whose tile boxes (expanded
        // by kGridCullEpsilon) intersect the snapshot frustum. Renders
        // nothing, so it is unit-testable without a device.
        [[nodiscard]] static Expected<GridTilePlan> planTiles(
            const CameraSnapshot &snapshot,
            std::span<const GridTileSubmission> submissions);

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code update(TimeSpan dt, const CameraSnapshot &camera_view);

        // The one draw path: plan (cull) -> resolve payloads (cache hit, or
        // stage cold from the submission) -> upload tile ring -> one
        // instanced draw (6 verts per quad, startInstanceLocation 0,
        // g_tile_base uniform carries the payload base because
        // SV_InstanceID is draw-local). Failure is fail-closed but NOT an
        // atomic frame: planning and payload upload are all-or-nothing, while
        // a group that cannot encode stops the frame with prior draws
        // standing (single draw here, so the frame is either whole or empty).
        [[nodiscard]] std::error_code render(const DrawContext &draw_context);

        // Additive multi-draw-indirect lane beside render(): the CPU-staged
        // drawInstanced path above keeps working untouched. Compaction is
        // pure CPU (compactIndirect, unit-testable without a device); the
        // encode binds the same pipeline/payloads and issues one drawIndirect
        // over the compacted args. No compute-cull lane, no pipeline changes,
        // no GI inputs of any kind.
        [[nodiscard]] static Expected<Array<rhi::IndirectDrawArguments> > compactIndirect(
            const GridTilePlan &plan);

        [[nodiscard]] std::error_code renderIndirect(const DrawContext &draw_context);

        // Infallible teardown: clears staged submissions, tile cache, and
        // counters, then releases GPU objects; always returns success.
        [[nodiscard]] std::error_code shutdown();

        // Device-loss hook: drops GPU objects (pipeline root, payload ring),
        // parks uploads, and drops staged submissions while RETAINING the
        // tile cache and counters so resolveCached keeps serving while
        // GPU-touching calls fail closed. Restart is shutdown + initialize.
        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        // Staged submissions (copied): fail-closed (invalid_argument) on a
        // malformed range or a reserved dirty-mask bit. Pure CPU: needs no
        // device, so device-free tests and the colony translator can stage
        // before (or without) initialize; only render touches the GPU.
        [[nodiscard]] std::error_code submitTiles(std::span<const GridTileSubmission> submissions);

        void clearTiles() noexcept;

        // Invalidates CPU tile/cell caches without discarding staged tiles.
        // Render-thread confined; keeps the GPU atlas alive until teardown,
        // and later slot rewrites require prepareCellUploads' idle fence.
        void clearCache() noexcept;

        // Dirty-chunk upload lane: writes the request view into the
        // pass-owned cache for exactly the dirty tiles of the staged
        // submission, and returns the uploaded tile count. Quiescent (clean)
        // chunks upload nothing and keep serving from the cache. Pure CPU
        // like submitTiles: needs no device. Render-thread confined like
        // every other mutator.
        [[nodiscard]] Expected<u32> requestUpload(const GridUploadRequest &request);

        // Call on the render thread BEFORE either render lane (never within a
        // render-pass callback). Only dirty visible cell chunks are published.
        // The callback is the renderer-owned graphics-queue idle fence, called
        // exactly once before writing an atlas read by earlier GPU work; a
        // clean/offscreen/flat-only frame never waits. Failure leaves chunks
        // dirty and both render lanes fail closed until a successful prepare.
        [[nodiscard]] std::error_code prepareCellUploads(rhi::IDevice &device,
                                                         std23::function_ref<std::error_code()> wait_for_gpu_idle);

        // Cache probe: the cached payload for a live entry. Quiescent tiles
        // are served here without touching the GPU. Stale or never-uploaded
        // chunk → invalid_argument.
        [[nodiscard]] Expected<GridTilePayload> resolveCached(u32 chunk_id) noexcept;

        [[nodiscard]] u64 uploadCount() const noexcept { return m_upload_count; }
        [[nodiscard]] u64 cacheHitCount() const noexcept { return m_cache_hit_count; }
        [[nodiscard]] u32 stagedTileCount() const noexcept { return safe_narrowing(m_submitted_tiles.size()); }
        [[nodiscard]] u32 cachedTileCount() const noexcept { return safe_narrowing(m_tile_cache.size()); }

    private:
        // ------------------------------------------------------------------
        // validation and planning
        // ------------------------------------------------------------------

        [[nodiscard]] static std::error_code validateSubmission_(const GridTileSubmission &submission) noexcept;

        // ------------------------------------------------------------------
        // resolve, upload, and encode
        // ------------------------------------------------------------------

        std::error_code createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);

        struct GridCachedPipeline {
            rhi::ComPtr<rhi::IRenderPipeline> m_pipeline{};
            rhi::ComPtr<rhi::IShaderObject> m_root_object{};
            // g_grid_frame's dereferenced ConstantBuffer cursor, resolved once
            // so the per-frame path skips the field lookup and sub-object walk.
            rhi::ShaderCursor m_frame_cursor{};
        };

        [[nodiscard]] Expected<GridCachedPipeline *> pipelineFor_(
            rhi::IDevice &device,
            const RenderPipelineSignature &signature);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor);

        [[nodiscard]] std::error_code ensureTilePayloads_(rhi::IDevice &device, u32 tile_count);

        [[nodiscard]] std::error_code ensureIndirectArgs_(rhi::IDevice &device, u32 arg_count);

        [[nodiscard]] std::error_code ensureCellBuffer_(rhi::IDevice &device, bool atlas_required);

        [[nodiscard]] std::error_code validateCellDraw_(const GridTilePlan &plan) const noexcept;

        void applyCellPayload_(GridTilePayload &payload, u32 chunk_id) const noexcept;

        // Drops the whole payload ring without touching a device: unmap only
        // runs when one is supplied (the growth/replacement path), while
        // shutdown and device loss release still-mapped buffers as-is.
        void releaseTilePayloads_(rhi::IDevice *device) noexcept;

        // Drops the whole indirect-args ring under the same contract as the
        // payload ring above. The shared ring cursor is owned by the payload
        // ring's release, so this one leaves it alone.
        void releaseIndirectArgs_(rhi::IDevice *device) noexcept;

        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        // The one render program: grid_tiles's tile-indexed vertex entry plus
        // the tint fragment entry. A signature change drops the cached
        // pipeline (and its persistent root object) alike.
        rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;
        std::optional<GridCachedPipeline> m_cached_pipeline;

        // The payload ring: kTileRingSize upload buffers at GridTilePayload
        // (32 B) stride, each created and mapped ONCE and kept mapped for its
        // whole life — render only advances the cursor, so a frame's writes
        // land in a buffer no in-flight frame is still reading. Sized like
        // the swapchain image count so one submit per frame never revisits a
        // slot still on the GPU. Pass-owned, released in shutdown before
        // waitOnHost.
        static constexpr u32 kTileRingSize{3u};
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_tile_payloads{};
        std::array<void *, kTileRingSize> m_tile_payload_mapped{};
        u32 m_tile_ring_cursor{0u};
        u64 m_tile_payload_capacity = 0u;

        // The compacted MDI args ring: one IndirectDrawArguments per visible
        // tile, written by the CPU each submit. Same 3-slot rotation (and the
        // same shared cursor) as the payload ring, so a frame's write never
        // lands in a buffer an in-flight frame is still reading.
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_indirect_args{};
        std::array<void *, kTileRingSize> m_indirect_args_mapped{};
        u64 m_indirect_args_capacity = 0u;

        // One persistently mapped shader-readable Upload atlas. Chunk n owns
        // words [n*8192,(n+1)*8192). Only visible pending chunks are written
        // after an explicit renderer queue fence. A 4-byte dummy SRV serves
        // the unchanged flat-only lane without allocating the 32 MiB atlas.
        rhi::ComPtr<rhi::IBuffer> m_cell_atlas{};
        void *m_cell_atlas_mapped = nullptr;
        rhi::ComPtr<rhi::IBuffer> m_cell_dummy{};

        struct GridCachedCells {
            std::unique_ptr<std::array<u32, kCellWordsPerChunk> > m_words{};
            bool m_pending = true;
        };

        FlatMap<u32, GridCachedCells> m_cell_cache{};

        // Pass-owned CPU tile cache + staged submissions. Render-thread
        // confined: no internal mutex; concurrent upload is a caller bug.
        struct GridCachedTile {
            GridTilePayload m_payload{};
            u32 m_material_id = 0u;
        };

        FlatMap<u32, GridCachedTile> m_tile_cache{};
        u64 m_upload_count = 0u;
        // Mutable so the const resolve probe can record hits; still
        // render-thread confined, never atomic.
        mutable u64 m_cache_hit_count = 0u;

        Array<GridTileSubmission> m_submitted_tiles{};

        CameraSnapshot m_camera_view;
        bool m_ready = false;
    };
}
