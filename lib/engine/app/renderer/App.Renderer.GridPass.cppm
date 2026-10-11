module;
#include "pP/Macros.h"
export module engine.app:renderer.grid_pass;

import :renderer.types;
import :renderer.frame_fence;
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

    // Owned numeric catalog visuals. Texture indices name the immutable world
    // bundle's source images; they are distinct from terrain material IDs.
    struct alignas(16) GridTerrainVisual {
        u32 m_material_id{};
        u32 m_color_rgba{};
        u32 m_texture_index{0xffffffffu};
        float m_texture_period_m{1.0f};
    };
    static_assert(sizeof(GridTerrainVisual) == 16u);

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
        static constexpr u32 kCellAtlasSlots{2u * kGridTileCacheCapacity};
        static constexpr u32 kCellAtlasWords{kCellAtlasSlots * kCellWordsPerChunk};

        // Indirect-lane vertex count: 6 non-indexed verts per tile quad, the
        // same geometry the CPU-staged drawInstanced path draws.
        static constexpr u32 kGridTileVertexCount{6u};

        // Per-frame constants: unjittered view-projection and visibility lookup
        // mode. Jitter never enters the pipeline key or frame upload.
        struct GridFrame {
            float4x4 m_view_projection{identity_v};
            u32 m_use_visible_ids{};
            float m_cell_size_m{1.0f};
            std::array<u32, 2u> m_padding{};
        };

        static_assert(sizeof(GridFrame) == 80u);

        // Pure CPU planner: keeps the submissions whose tile boxes (expanded
        // by kGridCullEpsilon) intersect the snapshot frustum. Renders
        // nothing, so it is unit-testable without a device.
        [[nodiscard]] static Expected<GridTilePlan> planTiles(
            const CameraSnapshot &snapshot, std::span<const GridTileSubmission> submissions, float cell_size_m = 1.0f);

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code update(TimeSpan dt, const CameraSnapshot &camera_view);

        // CPU reference draw path: plan (cull) -> resolve payloads (cache hit, or
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
        // over the CPU-compacted args. Retained as a reference lane.
        [[nodiscard]] static Expected<Array<rhi::IndirectDrawArguments>> compactIndirect(const GridTilePlan &plan);

        [[nodiscard]] std::error_code renderIndirect(const DrawContext &draw_context);

        // Production lane: reset/cull into per-slot IDs and one indirect draw
        // record before the render pass, then consume them without CPU readback.
        [[nodiscard]] std::error_code prepareGpuVisibility(const PreRenderContext &context);
        [[nodiscard]] std::error_code renderGpuIndirect(const DrawContext &context);

        // Test readback after the caller proves completion. Borrowed current-slot
        // buffers remain owned by this pass; production never reads them on CPU.
        [[nodiscard]] std::pair<rhi::IBuffer *, rhi::IBuffer *> visibilityBuffersForTest() const noexcept {
            const VisibilitySlot &slot = m_visibility_slots[m_tile_ring_cursor];
            return {slot.m_args.get(), slot.m_ids.get()};
        }

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

        // Owned per-frame staging payload, built on the CLIENT lane and
        // applied on the pass's owner lane (encoded pass-affinity: the
        // client never touches pass CPU state directly anymore). Plain
        // values + owned storage only — never a live grid or pass view.
        struct GridStagedUpload {
            u32 m_chunk_id = 0u;
            GridTilePayload m_payload{};
            Array<u16> m_cell_materials{};
        };

        struct GridStagedFrame {
            float m_cell_size_m{1.0f};
            bool m_has_catalog{false};
            u64 m_content_fingerprint{};
            Array<GridTerrainVisual> m_terrain_visuals{};
            Array<GridTileSubmission> m_tiles{};
            Array<GridStagedUpload> m_uploads{};
            // Dropped before staging: translator reset / regeneration /
            // fail-closed recovery. The client sets this; the lane applies
            // the clear so regeneration recovery never edits pass state
            // from main.
            bool m_reset_caches = false;
            // The client's single armed full-refresh flag at capture: if this
            // stage exits on the non-applied path (rejected, minimized,
            // cancelled, absorb-and-drop, worker error), the derived host
            // must re-arm its full-refresh flag so the retry re-stages the
            // world. The true value clears it at settlement.
            bool m_full_refresh{false};
            // Consumption-ack manifest for the driver dirty-set covered by
            // this frame's staging payload: (seed, chunk ids, revisions)
            // captured at the capture point. The lane round-trips it
            // into RenderFrameResult; the derived owner consumes it there
            // so the ack rides the reliable per-slot settlement path
            // (applied, not dropped, not errored) rather than staging.
            u64 m_ack_seed{};
            Array<u32> m_ack_chunk_ids{};
            Array<u64> m_ack_revisions{};
        };

        // Applies one owned staging payload on the pass's owner lane:
        // optional cache drop, then staged tiles, then dirty uploads (the
        // same validation the translator's synchronous path used). On an
        // upload failure the pass clears its staged tiles + cache and
        // returns the error; the client must full-refresh on recovery.
        [[nodiscard]] std::error_code applyStagedFrame(const GridStagedFrame &stage);

        // Explicit reference fixtures may synchronously prepare visible or all
        // chunks after the supplied queue-idle proof. Interactive rendering uses
        // the bounded, nonblocking publication method below.
        [[nodiscard]] std::error_code prepareCellUploads(
            rhi::IDevice &device, std23::function_ref<std::error_code()> wait_for_gpu_idle, bool all_tiles = false);

        // Interactive publication never waits. False retains the previous GPU
        // table and edit acknowledgments while a bounded cohort is prepared.
        [[nodiscard]] Expected<bool> tryPrepareCellUploads(
            rhi::IDevice &device, FenceCompletionValue completed, u32 chunk_budget = 8u);
        [[nodiscard]] u64 atlasDeferredCount() const noexcept {
            return m_atlas_deferred_count;
        }
        [[nodiscard]] u64 atlasUploadedBytes() const noexcept {
            return m_atlas_uploaded_bytes;
        }

        // Cache probe: the cached payload for a live entry. Quiescent tiles
        // are served here without touching the GPU. Stale or never-uploaded
        // chunk → invalid_argument.
        [[nodiscard]] Expected<GridTilePayload> resolveCached(u32 chunk_id) noexcept;

        // P1 fence settlement: records the fence value signalled by the
        // frame's SUBMIT for the ring slot this frame rotated onto. Called
        // by the frame owner after a successful submit; never called when
        // the submit failed (the slot keeps its older value, which is
        // conservative and safe: unwritten-or-stale contents are always
        // overwritten before any later use).
        void notifyFrameSubmitted(FenceCompletionValue submit_value) noexcept;

        // Telemetry for Gate 1: how often the atlas write took the
        // global-idle serialization, how many atlas writes happened, and how
        // often a ring growth waited for in-flight work.
        [[nodiscard]] u64 atlasSerializationCount() const noexcept {
            return m_atlas_serialization_count;
        }
        [[nodiscard]] u64 atlasWriteCount() const noexcept {
            return m_atlas_write_count;
        }

        [[nodiscard]] u64 residentTableBuildCount() const noexcept {
            return m_resident_table_build_count;
        }
        [[nodiscard]] u64 residentTableUploadCount() const noexcept {
            return m_resident_table_upload_count;
        }
        [[nodiscard]] u64 growthWaitCount() const noexcept {
            return m_growth_wait_count;
        }
        // Last-use completion per ring slot (shared cursor: one array covers
        // the payload ring and the indirect-args ring together).
        [[nodiscard]] std::array<FenceCompletionValue, 3u> slotCompletionValues() const noexcept {
            return m_tile_slot_values;
        }

        [[nodiscard]] u64 uploadCount() const noexcept {
            return m_upload_count;
        }
        [[nodiscard]] u64 cacheHitCount() const noexcept {
            return m_cache_hit_count;
        }
        [[nodiscard]] u32 stagedTileCount() const noexcept {
            return safe_narrowing(m_submitted_tiles.size());
        }
        [[nodiscard]] u32 cachedTileCount() const noexcept {
            return safe_narrowing(m_tile_cache.size());
        }

    private:
        // ------------------------------------------------------------------
        // validation and planning
        // ------------------------------------------------------------------

        [[nodiscard]] static std::error_code validateSubmission_(const GridTileSubmission &submission) noexcept;

        // ------------------------------------------------------------------
        // resolve, upload, and encode
        // ------------------------------------------------------------------

        // Payload-ring depth: 3 slots > 2 frame credits, so the selected
        // slot is complete without waiting whenever submissions retired in
        // order; the wait fallback covers the rest. Declared before the
        // pipeline entry below, whose per-slot roots are sized by it.
        static constexpr u32 kTileRingSize{3u};
        static_assert(kTileRingSize == kFrameRingSlots);

        std::error_code createShaderProgram_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        std::error_code createVisibilityPrograms_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        std::error_code ensureVisibilityBuffers_(rhi::IDevice &device);

        struct GridCachedPipeline {
            rhi::ComPtr<rhi::IRenderPipeline> m_pipeline{};
            // Per-slot ROOT shader objects (one per ring slot), each with
            // its own resolved g_grid_frame cursor. The frame writes only
            // its selected slot's cursor and binds only that slot's root,
            // so no submission ever observes another frame's constants —
            // the persistent-root mutation pattern is gone. A signature
            // change defers the whole entry (pipeline + all roots) through
            // m_deferred_releases instead of dropping it mid-flight.
            std::array<rhi::ComPtr<rhi::IShaderObject>, kTileRingSize> m_slot_roots{};
            std::array<rhi::ShaderCursor, kTileRingSize> m_slot_frame_cursors{};
        };

        [[nodiscard]] Expected<GridCachedPipeline *> pipelineFor_(
            rhi::IDevice &device, const RenderPipelineSignature &signature);

        // Reclaims deferred pipelines/roots at or below `completed`.
        void reclaimCompleted_(FenceCompletionValue completed) noexcept;

        // Rotates onto a completion-selected slot and waits it out when
        // stale. Records m_last_used_slot for notifyFrameSubmitted.
        [[nodiscard]] std::error_code selectSlot_(FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor);

        [[nodiscard]] std::error_code ensureTilePayloads_(
            rhi::IDevice &device, u32 tile_count, FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        [[nodiscard]] std::error_code ensureIndirectArgs_(
            rhi::IDevice &device, u32 arg_count, FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        [[nodiscard]] std::error_code ensureCellBuffer_(rhi::IDevice &device, bool atlas_required);

        [[nodiscard]] std::error_code validateCellDraw_(const GridTilePlan &plan) const noexcept;

        void applyCellPayload_(GridTilePayload &payload, u32 chunk_id) const noexcept;

        [[nodiscard]] std::error_code rebuildResidentTable_();
        [[nodiscard]] Expected<bool> prepareCellCohort_(
            rhi::IDevice &device, std::span<const u32> chunks, FenceCompletionValue completed, u32 chunk_budget);
        void cancelCellCohort_() noexcept;

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
        struct VisibilityProgram {
            rhi::ComPtr<rhi::IShaderProgram> m_program{};
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
            std::array<rhi::ComPtr<rhi::IShaderObject>, kTileRingSize> m_roots{};
        };
        struct VisibilitySlot {
            rhi::ComPtr<rhi::IBuffer> m_ids{};
            rhi::ComPtr<rhi::IBuffer> m_args{};
        };
        std::array<VisibilitySlot, kTileRingSize> m_visibility_slots{};
        std::array<VisibilityProgram, 2u> m_visibility_programs{};
        bool m_visibility_prepared{false};
        bool m_visibility_has_cells{false};
        float m_cell_size_m{1.0f};
        Array<GridTilePayload> m_resident_table{};
        bool m_resident_table_stale{true};
        std::array<bool, kTileRingSize> m_resident_table_uploaded{};
        u64 m_resident_table_build_count{};
        u64 m_resident_table_upload_count{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;
        std::optional<GridCachedPipeline> m_cached_pipeline;

        // The payload ring: kTileRingSize upload buffers at GridTilePayload
        // (32 B) stride, each created and mapped ONCE and kept mapped for its
        // whole life. The frame selects its slot by per-slot completion
        // values (m_tile_slot_values), never by blind cursor parity: with 3
        // slots and at most 2 outstanding submissions the selected slot is
        // complete without waiting, and the wait fallback below covers the
        // rest (including artificially delayed completion). Pass-owned,
        // released in shutdown before waitOnHost.
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_tile_payloads{};
        std::array<void *, kTileRingSize> m_tile_payload_mapped{};
        u32 m_tile_ring_cursor{0u};
        u64 m_tile_payload_capacity = 0u;
        // Shared-cursor coverage, once: the indirect-args ring rotates with
        // the payload ring on the same cursor, so ONE completion array
        // (m_tile_slot_values) governs both. A submission references both
        // slots together; they retire together.
        std::array<FenceCompletionValue, kTileRingSize> m_tile_slot_values{};
        u32 m_last_used_slot{0u};

        // Retired pipelines/roots that in-flight submissions may still
        // reference, keyed by last-use fence value. Reclaimed at every
        // render entry from the frame's completion value; drained
        // unconditionally at shutdown/device-loss.
        DeferredReleaseQueue m_deferred_releases{};
        FenceCompletionValue m_last_submitted_value{kFenceValueNone};

        // Gate 1 telemetry (see accessors above).
        u64 m_atlas_serialization_count{0u};
        u64 m_atlas_write_count{0u};
        u64 m_growth_wait_count{0u};

        // The compacted MDI args ring: one IndirectDrawArguments per visible
        // tile, written by the CPU each submit. Same 3-slot rotation on the
        // shared cursor as the payload ring (governed by the same
        // m_tile_slot_values, covered once), so a frame's write never lands
        // in a buffer an in-flight frame is still reading.
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_indirect_args{};
        std::array<void *, kTileRingSize> m_indirect_args_mapped{};
        u64 m_indirect_args_capacity = 0u;

        // Bounded 64 MiB Upload atlas: 2048 physical slots for 1024 logical
        // chunks. Published and retained-table slots stay pinned; retired slots
        // are reusable only after their last reader fence completes. A 4-byte
        // dummy serves flat-only rendering without allocating the atlas.
        rhi::ComPtr<rhi::IBuffer> m_cell_atlas{};
        void *m_cell_atlas_mapped = nullptr;
        rhi::ComPtr<rhi::IBuffer> m_cell_dummy{};
        std::array<std::optional<u32>, kGridTileCacheCapacity> m_cell_slots{};
        std::array<FenceCompletionValue, kCellAtlasSlots> m_cell_slot_values{};
        std::bitset<kCellAtlasSlots> m_cell_slots_current{};
        std::bitset<kCellAtlasSlots> m_resident_cell_slots{};
        std::bitset<kCellAtlasSlots> m_cell_slots_reserved{};
        Array<u32> m_cell_cohort_chunks{};
        Array<u32> m_cell_cohort_slots{};
        u32 m_cell_cohort_cursor{};
        bool m_cell_publication_deferred{false};
        u64 m_atlas_deferred_count{};
        u64 m_atlas_uploaded_bytes{};

        struct GridCachedCells {
            std::unique_ptr<std::array<u32, kCellWordsPerChunk>> m_words{};
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
