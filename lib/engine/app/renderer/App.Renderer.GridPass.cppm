module;
#include "pP/Macros.h"
export module engine.app:renderer.grid_pass;

import :renderer.types;
import :renderer.lighting;
import :renderer.lighting_pass;
import :renderer.world_radiance;
import :renderer.world_light_field;
import :renderer.frame_fence;
import :renderer.gpu_caches;
import :scene.camera;

import engine.core;
import engine.image;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    PPR_DECLARE_LOG_CATEGORY(GridPass)

    // ------------------------------------------------------------------
    // grid pass: chunk-sized tile quads over the cell world
    // ------------------------------------------------------------------
    // Content-specific pass owning its shader/pipeline/buffers/GPU tile
    // cache; Renderer stays content-free. Consumes CameraSnapshot (never a
    // mutable Camera while drawing); viewport and scissor travel per-draw on
    // DrawSubmission; row-major row-vector LH +Z-forward +Y-up, [0,1] depth,
    // mul(float4,matrix), view*projection, no backend transpose/Y-flip.
    // Content-owned immutable lighting leases are optional explicit inputs.
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
    inline constexpr u32 kGridSurfaceOpticalBlend = 1u;
    inline constexpr u32 kGridSurfaceTextureRepeat = 2u;
    inline constexpr u32 kGridSurfaceParticleSolid = 4u;
    inline constexpr u32 kGridSurfaceParticleLiquid = 8u;

    // Tagged version1 authored transport, independent of visual alpha/IOR/phase.
    inline constexpr u32 kGridTransportUnavailable = 0u;
    inline constexpr u32 kGridTransportVacuumV1 = 0x101u;
    inline constexpr u32 kGridTransportOpaqueV1 = 0x102u;
    inline constexpr u32 kGridTransportAbsorbingV1 = 0x103u;

    struct alignas(16) GridTerrainVisual {
        u32 m_material_id{};
        u32 m_color_rgba{};
        u32 m_texture_index{0xffffffffu};
        float m_texture_period_m{1.0f};
        float m_roundness{1.0f};
        // Derived from admitted albedo bytes when the immutable GPU palette is built.
        float m_filtered_mean_rgb[3]{1.0f, 1.0f, 1.0f};
        u32 m_normal_texture_index{0xffffffffu};
        float m_metallic{};
        float m_roughness{1.0f};
        float m_opacity{1.0f};
        float m_emissive_rgb[3]{};
        float m_emissive_strength{};
        float m_ior{1.0f};
        float m_normal_scale{1.0f};
        float m_refraction_depth_m{};
        u32 m_surface_flags{};
        u32 m_transport_model{kGridTransportUnavailable};
        float m_sigma_per_m[3]{};
    };
    static_assert(sizeof(GridTerrainVisual) == 96u);
    static_assert(std::is_standard_layout_v<GridTerrainVisual> and std::is_trivially_copyable_v<GridTerrainVisual>);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_transport_model) == 80u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_sigma_per_m) == 84u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_normal_texture_index) == 32u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_metallic) == 36u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_roughness) == 40u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_opacity) == 44u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_emissive_rgb) == 48u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_emissive_strength) == 60u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_ior) == 64u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_normal_scale) == 68u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_refraction_depth_m) == 72u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_surface_flags) == 76u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_roundness) == 16u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_filtered_mean_rgb) == 20u);
    static_assert(alignof(GridTerrainVisual) == 16u);

    // Visual-only unavailable rows remain admissible with zero sigma. Strict
    // optical conversion rejects unavailable; no missing material becomes vacuum.
    [[nodiscard]] std::error_code validateGridMaterialOptics(const GridTerrainVisual &visual);
    [[nodiscard]] std::expected<lighting::OpticalCell, std::error_code> gridMaterialOpticalCell(
        const GridTerrainVisual &visual);

    // ------------------------------------------------------------------
    // coarse material projection shared with immutable world sources
    // ------------------------------------------------------------------

    // One canonical coarsest admitted linear-image mean for rendering and
    // immutable world-source material projection; no texture data is retained.
    [[nodiscard]] Expected<std::array<float, 3u>> gridFilteredAlbedoMean(const image::ImageAsset &source);
    [[nodiscard]] Expected<std::array<float, 3u>> gridMaterialDiffuseAlbedo(
        const GridTerrainVisual &visual, std::span<const image::ImageAsset> images);

    struct alignas(16) GridBackgroundLayer {
        u32 m_texture_index{0xffffffffu};
        u32 m_order{};
        u32 m_color_rgba{0xffffffffu};
        float m_repeat_period_m{256.0f};
        float m_parallax_factor{};
        u32 m_reserved[3]{};
    };
    static_assert(sizeof(GridBackgroundLayer) == 32u);
    static_assert(PPR_OFFSETOF(GridBackgroundLayer, m_repeat_period_m) == 12u);
    static_assert(PPR_OFFSETOF(GridBackgroundLayer, m_parallax_factor) == 16u);

    inline constexpr u32 kGridSurfaceColumns = 4096u;
    using GridSurfaceHeights = std::array<std::array<float, 4u>, kGridSurfaceColumns / 4u>;
    static_assert(sizeof(GridSurfaceHeights) == 16384u);
    // Exact generation height at each fine-column centre. Immutable owned
    // presentation data; no generation recipe or simulation dependency crosses here.
    struct GridSurfaceProfile {
        float m_cell_size_m{1.0f};
        GridSurfaceHeights m_heights_m{};
    };

    // Initial-state admission seeds in physical units, before a resident solver
    // view exists. Uploaded once per changed inventory; never CPU draw geometry.
    struct GridParticleInstance {
        u64 m_id{};
        double m_position_x_m{};
        double m_position_y_m{};
        double m_volume_m3{};
        double m_mass_kg{};
        double m_temperature_k{};
        u32 m_material_id{};
    };
    struct GridParticleBatch {
        u32 m_chunk_id{};
        std::shared_ptr<const Array<GridParticleInstance>> m_particles{};
    };

    // ------------------------------------------------------------------
    // versioned resident-particle presentation input
    // ------------------------------------------------------------------

    [[nodiscard]] Expected<std::shared_ptr<const GridParticleMetadataResource>> prepareGridParticleMetadata(
        rhi::IDevice &device, std::span<const GridParticleMetadataRow> rows, u32 moving_count, double cell_metres,
        u64 version);

    // ------------------------------------------------------------------
    // capture-only committed particle projection
    // ------------------------------------------------------------------

    // Capture-only all-row projection. Zero means a valid half-open native
    // cell; flags preserve invalid/outside/inactive/vacuum rows without culling.
    enum EGridParticleCensusStatus : u32 {
        kGridParticleCensusInvalid = 1u,
        kGridParticleCensusOutside = 2u,
        kGridParticleCensusInactive = 4u,
        kGridParticleCensusVacuum = 8u,
    };

    struct GridParticleCensusRow {
        u32 m_id_low{}, m_id_high{}, m_material{}, m_status{};
        u32 m_cell_x{0xffff'ffffu}, m_cell_y{0xffff'ffffu};
    };
    static_assert(sizeof(GridParticleCensusRow) == 24u and alignof(GridParticleCensusRow) == 4u);
    static_assert(
        std::is_standard_layout_v<GridParticleCensusRow> and std::is_trivially_copyable_v<GridParticleCensusRow>);
    static_assert(PPR_OFFSETOF(GridParticleCensusRow, m_cell_x) == 16u);

    struct GridParticleCensus {
        std::vector<GridParticleCensusRow> m_rows{};
        // Weak identity; the selected view/capture owns the metadata resource.
        std::weak_ptr<const GridParticleMetadataResource> m_metadata_owner{};
        GridParticlePresentationIdentity m_identity{};
        FenceCompletionValue m_submission{kFenceValueNone};
        u64 m_device_epoch{}, m_device_bytes{}, m_readback_bytes{}, m_cpu_bytes{};
        u32 m_world_edge{};
    };

    enum class EGridDiagnosticMode : u32 {
        textured,
        material,
        biome,
        temperature,
        initial_matter
    };
    struct alignas(16) GridDiagnosticColor {
        u32 m_color_rgba{};
        float m_fill_height_fine{4096.0f};
        u32 m_reserved[2]{};
    };
    static_assert(sizeof(GridDiagnosticColor) == 16u);
    static_assert(alignof(GridDiagnosticColor) == 16u);
    static_assert(PPR_OFFSETOF(GridDiagnosticColor, m_color_rgba) == 0u);
    static_assert(PPR_OFFSETOF(GridDiagnosticColor, m_fill_height_fine) == 4u);
    static_assert(PPR_OFFSETOF(GridDiagnosticColor, m_reserved) == 8u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_material_id) == 0u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_color_rgba) == 4u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_texture_index) == 8u);
    static_assert(PPR_OFFSETOF(GridTerrainVisual, m_texture_period_m) == 12u);

    // Frozen boundary struct (plain data): one chunk-sized tile submission.
    // m_chunk_id is the row-major chunk index (the sim chunkIndexOf domain,
    // carried here as a plain u32). m_dirty_mask bit i marks tile i dirty;
    // chunk-sized tiles use bit 0 only and every higher bit is rejected.
    // m_material_id is the element/material tint index.
    struct GridTileSubmission {
        u64 m_dirty_mask = 0u;
        GridTileRange m_tile_range{};
        u32 m_chunk_id = 0u;
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

    // Owner-lane snapshot. Counts describe encoded commands and retained terrain
    // resources, not GPU completion, driver allocation size or total VRAM.
    struct GridPassObservations {
        u64 m_draw_commands{};
        u64 m_dispatch_commands{};
        u64 m_atlas_uploaded_bytes{};
        u64 m_atlas_writes{};
        u64 m_atlas_serializations{};
        u64 m_atlas_deferrals{};
        u64 m_growth_waits{};
        u64 m_table_builds{};
        u64 m_table_uploads{};
        u64 m_retirement_steps{};
        u64 m_retired_chunk_blocks{};
        u64 m_texture_entries{};
        u64 m_texture_slots{};
        u64 m_texture_inventory_comparisons{};
        u64 m_texture_inventory_reuses{};
        // Successful new cache entries and their source mip bytes; excludes
        // dedup hits, descriptors, palettes and driver allocation overhead.
        u64 m_texture_uploads{};
        u64 m_texture_uploaded_bytes{};
        u64 m_candidate_advances{};
        u32 m_retained_atlases{};
        u64 m_particle_uploads{};
        u64 m_particle_uploaded_bytes{};
        u64 m_particle_instances{};
        u64 m_particle_buffer_bytes{};
        u64 m_background_target_bytes{};
        u64 m_screen_contact_bytes{};
        u32 m_background_targets{};
        u32 m_screen_contact_masks{};
        u32 m_particle_root_bindings{};
        u64 m_texture_decoded_bytes{};
    };

    // ------------------------------------------------------------------
    // staged frame
    // ------------------------------------------------------------------
    // Namespace scope, not nested GridPass types: a module interface or type
    // definition outside engine.app that names a nested type of an imported
    // class makes the MSVC front end unable to materialize unrelated class
    // definitions in that translation unit (C1001, function-signature.cpp:213).
    // GridPass keeps the established nested spellings as aliases.
    struct GridStagedUpload;

    struct GridStagedFrame;

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

        // Per-frame raster view-projection and visibility lookup mode. The
        // render owner may supply temporal jitter; it never enters pipeline keys.
        struct GridFrame {
            float4x4 m_view_projection{identity_v};
            u32 m_use_visible_ids{};
            float m_cell_size_m{1.0f};
            u32 m_surface_mode{};
            u32 m_boundary_material_id{1u};
            float4x4 m_inverse_view_projection{identity_v};
            float m_viewport[4]{};
            float m_camera_origin[2]{};
            u32 m_has_background{};
            u32 m_has_surface{};
            std::array<GridBackgroundLayer, 4u> m_layers{};
        };

        static_assert(sizeof(GridFrame) == 304u);
        static_assert(alignof(GridFrame) == 16u);
        // Mango matrices have non-trivial copy/destruction; their established
        // row-major wire representation is pinned by these size/offset checks.
        static_assert(PPR_OFFSETOF(GridFrame, m_inverse_view_projection) == 80u);
        static_assert(PPR_OFFSETOF(GridFrame, m_viewport) == 144u);
        static_assert(PPR_OFFSETOF(GridFrame, m_layers) == 176u);
        static_assert(PPR_OFFSETOF(GridFrame, m_has_surface) == 172u);

        [[nodiscard]] std::error_code prepareBackground(
            const PreRenderContext &context, const CameraSnapshot &camera, const rhi::Viewport &viewport,
            const rhi::ScissorRect &scissor);
        [[nodiscard]] std::error_code renderBackground(const DrawContext &context);
        [[nodiscard]] std::error_code renderParticles(const DrawContext &context);

        // A host selects this view only after joint CPU/GPU commit. Old packets
        // are rejected on the owner lane before any upload/encoding touches it.
        [[nodiscard]] bool gpuParticleFrameCurrent(const std::optional<GridParticleGpuView> &view) const noexcept;
        [[nodiscard]] std::optional<GridParticlePresentationIdentity> activeParticlePresentation() const noexcept;
        [[nodiscard]] std::error_code applyGpuParticleView(const GridParticleGpuView &view);
        [[nodiscard]] std::error_code prepareGpuParticles(const PreRenderContext &context, u32 census_world_edge = 0u);
        [[nodiscard]] Expected<GridParticleCensus> readParticleCensus(
            FenceCompletionValue completed, FenceCompletionValue capture_submission);

        // Pure CPU planner: keeps the submissions whose tile boxes (expanded
        // by kGridCullEpsilon) intersect the snapshot frustum. Renders
        // nothing, so it is unit-testable without a device.
        [[nodiscard]] static Expected<GridTilePlan> planTiles(
            const CameraSnapshot &snapshot, std::span<const GridTileSubmission> submissions, float cell_size_m = 1.0f);

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

        // Visual continuation of completed receiver lighting across solid cut faces.
        enum class ReceiverContinuation : u32 {
            nearest,
            interpolated_experiment
        };

        enum class BackgroundResponse : u32 {
            unlit,
            lit_backwall,
            // Debug-only128x256 centre-density near-visibility pilot. Native
            // opaque/vacuum field required; foreground and world solver unchanged.
            screen_contact_pilot,
            screen_contact
        };

        enum class ReceiverResponse : u32 {
            flat,
            mapped_diffuse
        };

        // Explicit owned-frame sampling policy. Legacy temporal also uses x4,
        // so the attachment signature alone never selects this variant.
        [[nodiscard]] std::error_code update(
            TimeSpan dt, const CameraSnapshot &camera_view, bool spatial_msaa = false,
            ReceiverContinuation receiver_continuation = ReceiverContinuation::nearest,
            ReceiverResponse receiver_response = ReceiverResponse::flat,
            BackgroundResponse background_response = BackgroundResponse::unlit);
        [[nodiscard]] std::error_code validateSpatialContent() const noexcept;

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

        // Offscreen timing consumes five timestamps and leaves visible draws ready.
        static constexpr u32 kMaterialTimingBands = 16u;
        static constexpr u64 kMaterialTimingReserve = 16ull * 1024u * 1024u;
        [[nodiscard]] static bool supportsMaterialTiming(u32 width, u32 height) noexcept;
        [[nodiscard]] std::error_code measureTerrainMaterial(
            const PreRenderContext &context, u32 width, u32 height, u32 band, rhi::IQueryPool &queries,
            u32 first_query);

        // Test readback after the caller proves completion. Borrowed current-slot
        // buffers remain owned by this pass; production never reads them on CPU.
        [[nodiscard]] std::pair<rhi::IBuffer *, rhi::IBuffer *> visibilityBuffersForTest() const noexcept {
            const VisibilitySlot &slot = m_visibility_slots[m_tile_ring_cursor];
            return {slot.m_args.get(), slot.m_ids.get()};
        }

        // Borrowed diagnostics, valid only while the pass lives and after the
        // caller's submission fence completes. No production CPU field readback.
        [[nodiscard]] std::pair<rhi::ITexture *, rhi::IBuffer *> screenContactForTest() const noexcept {
            const auto &slot = m_background_slots[m_background_cursor];
            return {slot.m_screen_mask.get(), slot.m_screen_incident.get()};
        }

        [[nodiscard]] std::array<u32, 4u> screenContactRectForTest() const noexcept {
            const auto &p = m_background_slots[m_background_cursor].m_screen_parameters;
            return {p.m_left, p.m_top, p.m_count_x, p.m_count_y};
        }

        [[nodiscard]] std::array<rhi::IBuffer *, 3u> particleBuffersForTest() const noexcept {
            if (not m_world.m_gpu_particles) {
                return {};
            }

            const auto &particles = *m_world.m_gpu_particles;
            const auto &slot = particles.m_slots[particles.m_selected_slot];
            return {particles.m_packed.get(), slot.m_args.get(), slot.m_ids.get()};
        }

        // Diagnostic-only timestamp pairs: pack/cull, element splat, density
        // splat, filter, composite, ordinary particles. The caller permits one
        // instrumented frame in flight and resets/reads after its exact fence.
        [[nodiscard]] std::error_code setParticleTimingQueriesForTest(
            rhi::ComPtr<rhi::IQueryPool> queries, FenceCompletionValue completed);

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
        // and slot rewrites require completed reader-fence proof.
        void clearCache() noexcept;

        // Dirty-chunk upload lane: writes the request view into the
        // pass-owned cache for exactly the dirty tiles of the staged
        // submission, and returns the uploaded tile count. Quiescent (clean)
        // chunks upload nothing and keep serving from the cache. Pure CPU
        // like submitTiles: needs no device. Render-thread confined like
        // every other mutator.
        [[nodiscard]] Expected<u32> requestUpload(const GridUploadRequest &request);

        /// Staged upload row carried by a staged frame; the type itself lives at
        /// namespace scope (see below) and keeps the established nested spelling.
        using GridStagedUpload = ::pP::GridStagedUpload;

        /// Staged frame applied by this pass; the type itself lives at namespace
        /// scope (see below) and keeps the established nested spelling here.
        using GridStagedFrame = ::pP::GridStagedFrame;

        // Applies one owned staging payload on the pass's owner lane:
        // optional cache drop, then staged tiles, then dirty uploads (the
        // same validation the translator's synchronous path used). On an
        // upload failure the pass clears its staged tiles + cache and
        // returns the error; the client must full-refresh on recovery.
        [[nodiscard]] std::error_code applyStagedFrame(const GridStagedFrame &stage);

        // Interactive publication never waits. False retains the previous GPU
        // table and edit acknowledgments while a bounded cohort is prepared.
        [[nodiscard]] Expected<bool> tryPrepareCellUploads(
            rhi::IDevice &device, FenceCompletionValue completed, u32 chunk_budget = 8u);

        // Owner-lane candidate preparation retains the active world. Identity
        // must match at activation; at most active+candidate or active+retiring
        // resource bundles exist. Readiness alone never activates a candidate.
        [[nodiscard]] std::error_code stageWorldCandidate(
            u64 job_id, u64 world_generation, const GridStagedFrame &stage, bool uploads_complete = true);
        [[nodiscard]] std::error_code appendWorldCandidate(
            u64 job_id, u64 world_generation, std::span<const GridStagedUpload> uploads, bool final_batch);
        [[nodiscard]] Expected<bool> advanceWorldCandidate(
            rhi::IDevice &device, FenceCompletionValue completed, u32 chunk_budget = 8u);
        [[nodiscard]] std::error_code activateWorldCandidate(u64 job_id, u64 world_generation, u64 fingerprint);
        void cancelWorldCandidate(u64 job_id) noexcept;
        [[nodiscard]] bool worldCandidateAdmissionReady(FenceCompletionValue completed) noexcept;
        [[nodiscard]] bool worldCandidateMatches(u64 job_id, u64 world_generation, u64 fingerprint) const noexcept;
        [[nodiscard]] u64 worldGeneration() const noexcept {
            return m_world.m_world_generation;
        }

        // Accept only a completed immutable receiver generation whose geometry
        // attachment is the actual resident atlas/table publication. Ring roots
        // retain strong leases until their accepted draw completion retires.
        [[nodiscard]] std::error_code setTerrainReceiverLighting(
            std::shared_ptr<const lighting::TerrainReceiverGpuGeneration> generation);
        [[nodiscard]] std::error_code setTerrainBackgroundLighting(
            std::shared_ptr<const lighting::TerrainBackgroundGpuView> view);

        // Small-scene reference only: same checked native source and actual
        // resident mapping, never a camera-specific screen-light buffer.
        [[nodiscard]] std::error_code setWorldRadianceLighting(
            std::shared_ptr<const lighting::WorldRadianceGeneration> generation);

        // Borrowed temporal world light field view (Task 8): the current radiance
        // buffer and CPU constants are bound each frame for the opaque terrain
        // path. The field owner outlives every view; pass an empty view to clear.
        [[nodiscard]] std::error_code setWorldLightField(const lighting::WorldLightFieldView &view);

        [[nodiscard]] GridWorldIdentity activeWorldIdentity() const noexcept {
            return {m_world.m_job_id, m_world.m_world_generation, m_world.m_content_fingerprint};
        }

        [[nodiscard]] u32 retainedAtlasCountForTest() const noexcept;
        [[nodiscard]] GridPassObservations observations() const noexcept;
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

        // The retired queue-idle preparation path leaves zero serializations.
        // Keep that historical telemetry query alongside actual writes/growth waits.
        [[nodiscard]] u64 atlasSerializationCount() const noexcept {
            return 0u;
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
            return safe_narrowing(m_world.m_submitted_tiles.size());
        }
        [[nodiscard]] u32 cachedTileCount() const noexcept {
            return safe_narrowing(m_world.m_tile_cache.size());
        }

    private:
        // ------------------------------------------------------------------
        // validation and planning
        // ------------------------------------------------------------------

        [[nodiscard]] static std::error_code validateSubmission_(const GridTileSubmission &submission) noexcept;

        [[nodiscard]] bool worldRadianceSourceCompatible_(const lighting::WorldRadianceSource &source) const noexcept;

        [[nodiscard]] static std::error_code validateOpticalAttachment_(
            const GridStagedFrame &stage, bool &diffuse_compatible);
        struct GridCachedPipeline;
        [[nodiscard]] std::error_code bindReceiverLighting_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, bool retained_table, GridCachedPipeline *pipeline,
            u32 owner_slot);
        [[nodiscard]] std::error_code bindWorldRadiance_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, GridCachedPipeline *pipeline, u32 slot, bool background,
            std::shared_ptr<const lighting::WorldRadianceGeneration> *background_lease = nullptr);
        [[nodiscard]] std::error_code bindBackgroundLighting_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, GridCachedPipeline &pipeline, u32 slot,
            const CameraSnapshot &camera, int2 extent);
        [[nodiscard]] std::error_code bindWorldLightField_(rhi::ShaderCursor &cursor);


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
        std::error_code ensureSurfaceBuffers_(rhi::IDevice &device);
        [[nodiscard]] std::error_code bindMaterialBuffers_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, bool retained_table);
        std::error_code bindSurfaceBuffers_(
            rhi::IDevice &device, rhi::ShaderCursor &cursor, bool retained_table,
            GridCachedPipeline *pipeline = nullptr, std::optional<u32> owner_slot = {});
        [[nodiscard]] Expected<bool> prepareTerrainVisuals_(rhi::IDevice &device, u32 image_budget);
        [[nodiscard]] std::error_code ensureTerrainSampler_(rhi::IDevice &device);
        [[nodiscard]] std::error_code stageDiagnostic_(const GridStagedFrame &stage);
        [[nodiscard]] std::error_code appendDiagnostic_(const GridStagedUpload &upload);
        [[nodiscard]] Expected<bool> prepareDiagnostic_(rhi::IDevice &device, u32 chunk_budget);

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
            std::array<std::vector<std::shared_ptr<const lighting::TerrainReceiverGpuGeneration>>, kTileRingSize>
                m_slot_receiver_leases{};
            std::array<std::vector<std::shared_ptr<const lighting::TerrainBackgroundGpuView>>, kTileRingSize>
                m_slot_background_leases{};
            std::array<std::vector<std::shared_ptr<const lighting::WorldRadianceGeneration>>, kTileRingSize>
                m_slot_world_leases{};
            std::array<std::vector<rhi::ComPtr<rhi::ITextureView>>, kTileRingSize> m_slot_backdrop_owners{};
            std::array<rhi::IBuffer *, kTileRingSize> m_slot_atlases{};
            std::array<FenceCompletionValue, kTileRingSize> m_slot_last_use{};
            std::array<u64, kTileRingSize> m_slot_world_generation{};
            std::array<u64, kTileRingSize> m_slot_world_job_id{};
            std::array<bool, kTileRingSize> m_slot_encoded{};
            std::array<rhi::IBuffer *, kTileRingSize> m_slot_particle_buffers{};
            std::array<rhi::ITextureView *, kTileRingSize> m_slot_backdrop_views{};
            std::array<bool, kTileRingSize> m_slot_stale_bindings{};
        };

        [[nodiscard]] std::error_code ensurePipelineRoots_(rhi::IDevice &device, GridCachedPipeline &pipeline);

        struct ParticleGpu {
            float m_position[2]{};
            float m_radius_m{};
            u32 m_material_id{};
            float m_color[4]{};
        };
        static_assert(sizeof(ParticleGpu) == 32u and alignof(ParticleGpu) == 4u);
        static_assert(PPR_OFFSETOF(ParticleGpu, m_material_id) == 12u);
        static_assert(PPR_OFFSETOF(ParticleGpu, m_color) == 16u);

        struct ParticleChunk {
            std::shared_ptr<const Array<GridParticleInstance>> m_source{};
            bool m_dirty{};
        };

        // Each completed particle slot owns its screen-space fluid attachments
        // and roots. Resize never replaces resources still read by another slot.
        struct FluidSlot {
            std::array<rhi::ComPtr<rhi::ITexture>, 4u> m_textures{};
            std::array<rhi::ComPtr<rhi::ITextureView>, 4u> m_views{};
            std::array<rhi::ComPtr<rhi::IShaderObject>, 3u> m_roots{};
            std::optional<GridCachedPipeline> m_composite_pipeline{};
            std::optional<RenderPipelineKey> m_composite_key{};
            u32 m_width{}, m_height{};
        };

        // Solids retain their own occupancy and nearest-element attachments
        // alongside liquids until both surfaces have been composited.
        struct SolidSlot {
            std::array<rhi::ComPtr<rhi::ITexture>, 3u> m_textures{};
            std::array<rhi::ComPtr<rhi::ITextureView>, 3u> m_views{};
            std::array<rhi::ComPtr<rhi::IShaderObject>, 2u> m_roots{};
            std::optional<GridCachedPipeline> m_composite_pipeline{};
            std::optional<RenderPipelineKey> m_composite_key{};
            u32 m_width{}, m_height{};
        };

        struct ParticleCensusStorage {
            rhi::ComPtr<rhi::IBuffer> m_projected{}, m_readback{};
            rhi::ComPtr<rhi::IShaderObject> m_root{};
            rhi::IDevice *m_device{}; // Actual allocator; device outlives pass slots.
            GridParticlePresentationIdentity m_identity{};
            FenceCompletionValue m_submission{kFenceValueNone};
            u64 m_device_epoch{};
            u32 m_count{}, m_world_edge{};
            bool m_encoded{};
        };

        struct GpuParticleSlot {
            rhi::ComPtr<rhi::IBuffer> m_ids{}, m_args{};
            std::optional<GridParticleGpuView> m_reader{};
            std::unique_ptr<FluidSlot> m_fluid{};
            std::unique_ptr<SolidSlot> m_solid{};
            std::unique_ptr<ParticleCensusStorage> m_census{};
        };

        struct GpuParticleStorage {
            std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, kTileRingSize>, 4u> m_roots{};
            std::array<GpuParticleSlot, kTileRingSize> m_slots{};
            rhi::ComPtr<rhi::IBuffer> m_packed{}, m_ranks{}, m_counts{}, m_prefix{};

            GridParticleGpuView m_view{};
            std::shared_ptr<const std::atomic<u64>> m_packed_owner{}, m_pending_owner{};

            u64 m_packed_epoch{std::numeric_limits<u64>::max()}, m_packed_metadata{}, m_packed_terrain{};
            u64 m_pending_epoch{}, m_pending_metadata{}, m_pending_terrain{};
            u32 m_capacity{}, m_selected_slot{};
            bool m_pending_pack{}, m_prepared{}, m_slot_encoded{};
        };

        struct ScreenContactParameters {
            u32 m_width{}, m_height{}, m_left{}, m_top{};
            u32 m_count_x{}, m_count_y{}, m_enabled{}, m_stride{};
        };
        static_assert(sizeof(ScreenContactParameters) == 32u and alignof(ScreenContactParameters) == 4u);
        static_assert(PPR_OFFSETOF(ScreenContactParameters, m_count_x) == 16u);
        static_assert(
            std::is_standard_layout_v<ScreenContactParameters> and
            std::is_trivially_copyable_v<ScreenContactParameters>);
        static_assert(PPR_OFFSETOF(ScreenContactParameters, m_enabled) == 24u);
        static_assert(PPR_OFFSETOF(ScreenContactParameters, m_stride) == 28u);

        struct BackgroundSlot {
            std::shared_ptr<const lighting::TerrainBackgroundGpuView> m_lighting{};
            std::shared_ptr<const lighting::WorldRadianceGeneration> m_world_lighting{};
            rhi::ComPtr<rhi::ITexture> m_texture{};
            rhi::ComPtr<rhi::ITextureView> m_view{};
            rhi::ComPtr<rhi::ITexture> m_screen_mask{};
            rhi::ComPtr<rhi::ITextureView> m_screen_mask_view{};
            rhi::ComPtr<rhi::IBuffer> m_screen_incident{};
            rhi::ComPtr<rhi::IShaderObject> m_screen_root{};
            std::shared_ptr<const lighting::WorldRadianceGeneration> m_screen_lighting{};
            FenceCompletionValue m_last_use{kFenceValueNone};
            ScreenContactParameters m_screen_parameters{};
            u32 m_width{};
            u32 m_height{};
            bool m_screen_encoded{};
        };

        [[nodiscard]] std::error_code ensureScreenContactPrograms_(rhi::IDevice &device);
        [[nodiscard]] std::error_code prepareScreenContact_(
            const PreRenderContext &context, const CameraSnapshot &camera, u32 selected, u32 width, u32 height);
        [[nodiscard]] std::error_code bindScreenContact_(rhi::ShaderCursor &cursor, u32 selected);

        [[nodiscard]] std::error_code validateParticles_(const GridStagedFrame &stage) const;
        void stageParticles_(const GridStagedFrame &stage);
        void markPipelineUse_(GridCachedPipeline &pipeline, u32 slot) noexcept;
        [[nodiscard]] std::error_code admitParticleSeeds_(rhi::IDevice &device);
        [[nodiscard]] std::error_code createGpuParticlePrograms_(
            IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code ensureParticleMask_(rhi::IDevice &device);
        [[nodiscard]] std::error_code createFluidPrograms_(
            IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code prepareFluidSurface_(const PreRenderContext &context);
        [[nodiscard]] std::error_code renderFluidSurface_(const DrawContext &context, u32 slot);
        [[nodiscard]] bool hasLiquidParticles_() const noexcept;
        [[nodiscard]] std::error_code createSolidPrograms_(
            IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code prepareSolidSurface_(const PreRenderContext &context);
        [[nodiscard]] std::error_code renderSolidSurface_(const DrawContext &context, u32 slot);
        [[nodiscard]] bool hasSolidParticles_() const noexcept;
        [[nodiscard]] std::error_code ensureGpuParticleStorage_(rhi::IDevice &device, u32 count);
        [[nodiscard]] std::error_code renderGpuParticles_(const DrawContext &context);
        void prepareChunkSlotMap_(bool retained_table);
        [[nodiscard]] std::error_code createOpticalPrograms_(
            IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] Expected<GridCachedPipeline *> opticalPipeline_(
            rhi::IDevice &device, const RenderPipelineSignature &signature, rhi::IShaderProgram *program,
            std::optional<RenderPipelineKey> &key, std::optional<GridCachedPipeline> &cache, bool blend,
            bool premultiplied = false);
        void fillOpticalFrame_(GridFrame &frame) const noexcept;
        void clearOpticalResources_() noexcept;
        [[nodiscard]] std::error_code ensureSpatialPrograms_(rhi::IDevice &device, bool reactive = false);

        [[nodiscard]] std::error_code encodeTerrainIndirect_(
            const DrawContext &context, GridCachedPipeline &pipeline, rhi::IQueryPool *queries = nullptr,
            u32 first_query = 0u);

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
        rhi::ComPtr<rhi::IShaderProgram> m_spatial_shader_program{};

        // Optional native MRT variants share the same fenced roots and caches.
        rhi::ComPtr<rhi::IShaderProgram> m_reactive_shader_program{}, m_reactive_background_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_reactive_particle_program{}, m_reactive_fluid_program{},
            m_reactive_solid_program{};
        safe_ptr<IShaderService> m_shader_service{};
        std::filesystem::path m_content_dir{};
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

        struct GpuParticleProgram {
            rhi::ComPtr<rhi::IShaderProgram> m_program{};
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        };
        std::array<GpuParticleProgram, 4u> m_particle_prepare_programs{};
        GpuParticleProgram m_particle_census_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_gpu_particle_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_gpu_particle_spatial_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_fluid_spatial_program{}, m_solid_spatial_program{};
        std::optional<RenderPipelineKey> m_gpu_particle_key{};
        std::optional<GridCachedPipeline> m_gpu_particle_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 4u> m_fluid_programs{};
        std::array<rhi::ComPtr<rhi::IRenderPipeline>, 2u> m_fluid_splat_pipelines{};
        rhi::ComPtr<rhi::IComputePipeline> m_fluid_filter_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 3u> m_solid_programs{};
        std::array<rhi::ComPtr<rhi::IRenderPipeline>, 2u> m_solid_splat_pipelines{};
        rhi::ComPtr<rhi::IQueryPool> m_particle_timing_queries{};
        bool m_visibility_prepared{false};


        rhi::ComPtr<rhi::IBuffer> m_contour_points{};
        rhi::ComPtr<rhi::IBuffer> m_contour_cases{};
        // Dummy UAV for the world light field radiance binding when no field is set.
        rhi::ComPtr<rhi::IBuffer> m_world_light_dummy{};
        std::array<rhi::ComPtr<rhi::IBuffer>, kTileRingSize> m_chunk_slot_buffers{};
        std::array<void *, kTileRingSize> m_chunk_slot_mapped{};
        std::array<bool, kTileRingSize> m_chunk_slot_uploaded{};
        rhi::ComPtr<rhi::ITexture> m_particle_mask{};
        rhi::ComPtr<rhi::ITextureView> m_particle_mask_view{};
        rhi::ComPtr<rhi::ISampler> m_particle_mask_sampler{};
        rhi::ComPtr<rhi::ITexture> m_boundary_noise{};
        rhi::ComPtr<rhi::ITextureView> m_boundary_noise_view{};
        rhi::ComPtr<rhi::ITexture> m_terrain_fallback{};
        rhi::ComPtr<rhi::ITextureView> m_terrain_fallback_view{};
        rhi::ComPtr<rhi::ISampler> m_terrain_sampler{};
        rhi::DescriptorHandle m_terrain_fallback_handle{};
        rhi::DescriptorHandle m_terrain_sampler_handle{};


        std::array<bool, kTileRingSize> m_resident_table_uploaded{};
        u64 m_draw_commands{};
        u64 m_dispatch_commands{};
        u64 m_retirement_steps{};
        u64 m_retired_chunk_blocks{};
        u64 m_texture_inventory_comparisons{};
        u64 m_texture_inventory_reuses{};
        u64 m_texture_uploads{};
        u64 m_texture_uploaded_bytes{};
        u64 m_candidate_advances{};
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


        u64 m_atlas_deferred_count{};
        u64 m_atlas_uploaded_bytes{};

        struct GridCachedCells {
            std::unique_ptr<std::array<u32, kCellWordsPerChunk>> m_words{};
            bool m_pending = true;
        };


        // Pass-owned CPU tile cache + staged submissions. Render-thread
        // confined: no internal mutex; concurrent upload is a caller bug.
        struct GridCachedTile {
            GridTilePayload m_payload{};
            u32 m_material_id = 0u;
        };


        u64 m_upload_count = 0u;
        // Mutable so the const resolve probe can record hits; still
        // render-thread confined, never atomic.
        mutable u64 m_cache_hit_count = 0u;


        // ------------------------------------------------------------------
        // sealed ordered terrain texture inventory
        // ------------------------------------------------------------------

        // Private while uploading; shared only after sealing. The cache owns
        // descriptor storage and pinned GPU images, and these handles/slots
        // are meaningful only with that cache. No slots are added after seal.
        struct TerrainTextureInventory {
            Array<image::ImageAsset> m_images{};
            Array<u32> m_slots{};
            Array<TextureHandle> m_handles{};
            std::unique_ptr<BindlessTextureCache> m_cache{};
            rhi::IDevice *m_device{}; // Identity only; device outlives every world.

            u32 m_cursor{};
            bool m_sealed{};
            bool m_reuse_checked{};
        };

        // ------------------------------------------------------------------
        // independently published world resources
        // ------------------------------------------------------------------

        // One world owns its palette, atlas mappings and retained draw
        // state. Candidate preparation can exchange this value on the owner
        // lane without moving pass programs, completion rings or the camera.
        struct WorldResources {
            std::shared_ptr<const lighting::OpticalTerrainSnapshot> m_optical_staged{}, m_optical_mapped{},
                m_optical_resident{};
            std::shared_ptr<const lighting::WorldRadianceSource> m_radiance_staged{}, m_radiance_mapped{},
                m_radiance_resident{};
            std::shared_ptr<const lighting::WorldRadianceGeneration> m_world_lighting{};
            u64 m_radiance_staged_revision{}, m_radiance_mapped_revision{}, m_radiance_resident_revision{};
            std::shared_ptr<const lighting::TerrainReceiverGpuGeneration> m_receiver_lighting{};
            std::shared_ptr<const lighting::TerrainBackgroundGpuView> m_background_lighting{};
            u64 m_optical_staged_revision{}, m_optical_mapped_revision{}, m_optical_resident_revision{};
            std::unique_ptr<GpuParticleStorage> m_gpu_particles{};
            std::shared_ptr<std::atomic<u64>> m_initial_particle_epoch{};
            u64 m_particle_seed_version{};
            bool m_particle_seeds_dirty{};
            // Derived material compatibility travels with each existing source
            // mapping; the source/cohort and completed GPU mapping stay authority.
            bool m_radiance_staged_diffuse{}, m_radiance_mapped_diffuse{}, m_radiance_resident_diffuse{};
            bool m_has_background{};
            std::array<GridBackgroundLayer, 4u> m_background_layers{};
            std::shared_ptr<const GridSurfaceProfile> m_surface_profile{};
            float m_reference_thickness_m{1.0f};
            std::array<ParticleChunk, kGridTileCacheCapacity> m_particles{};
            EGridDiagnosticMode m_diagnostic_mode{EGridDiagnosticMode::textured};
            u64 m_diagnostic_revision{};
            Array<GridDiagnosticColor> m_diagnostic_palette{};
            std::array<Array<u32>, kGridTileCacheCapacity> m_diagnostic_words{};
            u32 m_diagnostic_cursor{};
            bool m_initial_matter_valid{true};
            bool m_initial_matter_invalidation_pending{};
            rhi::ComPtr<rhi::IBuffer> m_diagnostic_atlas{};
            void *m_diagnostic_mapped{};
            rhi::ComPtr<rhi::IBuffer> m_diagnostic_colors{};
            bool m_candidate_uploads_complete{true};
            bool m_upload_submission_pending{false};
            bool m_visuals_set{false};
            Array<GridTerrainVisual> m_terrain_visuals{};
            std::shared_ptr<TerrainTextureInventory> m_texture_inventory{};

            u32 m_retirement_diagnostic_cursor{};
            u32 m_retirement_particle_cursor{};
            bool m_retirement_roots_detached{};
            rhi::ComPtr<rhi::IBuffer> m_terrain_palette{};
            u64 m_job_id{};
            u64 m_world_generation{};
            u64 m_content_fingerprint{};
            FenceCompletionValue m_last_reader{kFenceValueNone};
            bool m_prepared{false};
            bool m_visibility_has_cells{false};
            bool m_contours_enabled{false};
            bool m_resident_contours_enabled{false};
            float m_resident_cell_size_m{1.0f};
            u32 m_boundary_material_id{1u};
            u32 m_resident_boundary_material_id{1u};
            std::array<u32, kGridTileCacheCapacity> m_resident_chunk_slots{};
            float m_cell_size_m{1.0f};
            Array<GridTilePayload> m_resident_table{};
            bool m_resident_table_stale{true};
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
            FlatMap<u32, GridCachedCells> m_cell_cache{};
            FlatMap<u32, GridCachedTile> m_tile_cache{};
            Array<GridTileSubmission> m_submitted_tiles{};
        };
        WorldResources m_world{};
        lighting::WorldLightFieldView m_world_light_view{};
        std::unique_ptr<WorldResources> m_candidate_world{};
        std::unique_ptr<WorldResources> m_retiring_world{};

        CameraSnapshot m_camera_view;
        std::array<BackgroundSlot, kTileRingSize> m_background_slots{};
        u32 m_background_cursor{};
        bool m_background_ready{};
        bool m_surface_retained_table{};
        rhi::Viewport m_background_viewport{};
        rhi::ScissorRect m_background_scissor{};
        CameraSnapshot m_background_camera{};
        rhi::ComPtr<rhi::IShaderProgram> m_background_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_background_copy_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_background_spatial_program{};
        std::optional<RenderPipelineKey> m_background_key{}, m_background_copy_key{};
        std::optional<GridCachedPipeline> m_background_pipeline{}, m_background_copy_pipeline{};
        std::optional<RenderPipelineKey> m_background_spatial_key{};
        std::optional<GridCachedPipeline> m_background_spatial_pipeline{};

        rhi::ComPtr<rhi::IShaderProgram> m_screen_mask_program{}, m_screen_refine_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_screen_refine_pipeline{};
        std::optional<RenderPipelineKey> m_screen_mask_key{};
        std::optional<GridCachedPipeline> m_screen_mask_pipeline{};
        std::optional<RenderPipelineKey> m_material_cost_key{};
        std::optional<GridCachedPipeline> m_material_cost_pipeline{};
        std::array<rhi::ComPtr<rhi::ITexture>, kTileRingSize> m_material_cost_targets{};
        std::array<rhi::ComPtr<rhi::ITextureView>, kTileRingSize> m_material_cost_views{};

        std::array<u32, 6u> m_screen_logged_region{};

        u64 m_particle_uploads{};
        u64 m_particle_uploaded_bytes{};
        ReceiverContinuation m_receiver_continuation{ReceiverContinuation::nearest};
        ReceiverResponse m_receiver_response{ReceiverResponse::flat};
        BackgroundResponse m_background_response{BackgroundResponse::unlit};
        bool m_cached_spatial_msaa{};
        bool m_spatial_msaa{};
        bool m_spatial_supported{};
        bool m_ready = false;
    };

    // ------------------------------------------------------------------
    // staged frame
    // ------------------------------------------------------------------

    /// One staged tile upload carried by a staged frame.
    struct GridStagedUpload {
        u32 m_chunk_id = 0u;
        GridTilePayload m_payload{};
        Array<u16> m_cell_materials{};
        Array<u16> m_diagnostic_indices{};
    };

    /// Owned per-frame staging payload, built on the CLIENT lane and applied on
    /// the pass's owner lane (encoded pass-affinity: the client never touches
    /// pass CPU state directly anymore). Plain values + owned storage only —
    /// never a live grid or pass view.
    struct GridStagedFrame {
        // Render the retained grid and actors while the client session is frozen.
        bool m_retain_content{false};
        // Static terrain can retain its checked cohort while the ordinary game
        // actor overlay advances; frozen-session callers keep the default false.
        bool m_update_actors_on_retained_terrain{};
        // Per-frame default-off presentation experiment; never source/world state.
        bool m_screen_contact_pilot{};
        // A game-clock update for the requested terminal sky on the render lane.
        std::optional<i32> m_temporal_sky_hour{};
        lighting::TerrainPrecisionSelection m_lighting_precision{};
        float m_cell_size_m{1.0f};
        bool m_has_catalog{false};
        u32 m_boundary_material_id{1u};
        u64 m_content_fingerprint{};

        // Complete bounded attachment, checked against these same uploads and
        // selected visual rows before publication; requires a full cache reset
        // so no unchecked neighboring slot survives into its lookup mapping.
        std::shared_ptr<const lighting::OpticalTerrainSnapshot> m_optical_geometry{};
        u64 m_optical_revision{};

        // Optional owned CPU producer request. Grid validates the same geometry;
        // ApplicationEditor owns computation, completion and GPU publication.
        std::shared_ptr<const lighting::TerrainReceiverSet> m_lighting_receivers{};

        // Mutually exclusive native transport attachment. Same complete reset,
        // selected palette, actual uploads and resident publication contract.
        std::shared_ptr<const lighting::WorldRadianceSource> m_world_radiance_source{};
        u64 m_world_radiance_revision{};

        Array<GridTerrainVisual> m_terrain_visuals{};
        Array<image::ImageAsset> m_terrain_images{};
        bool m_has_background{false};
        std::array<GridBackgroundLayer, 4u> m_background_layers{};
        std::shared_ptr<const GridSurfaceProfile> m_surface_profile{};
        float m_reference_thickness_m{1.0f};
        Array<GridParticleBatch> m_particle_batches{};
        std::optional<GridParticleGpuView> m_gpu_particles{};
        u32 m_capture_particle_census_edge{}; // Capture diagnostic only; zero disables.
        Array<GridTileSubmission> m_tiles{};
        Array<GridStagedUpload> m_uploads{};
        EGridDiagnosticMode m_diagnostic_mode{EGridDiagnosticMode::textured};
        u64 m_diagnostic_revision{};
        Array<GridDiagnosticColor> m_diagnostic_palette{};
        bool m_initial_matter_valid{true};
        // Dropped before staging: translator reset / regeneration / fail-closed
        // recovery. The client sets this; the lane applies the clear so
        // regeneration recovery never edits pass state from main.
        bool m_reset_caches = false;
        // The client's single armed full-refresh flag at capture: if this stage
        // exits on the non-applied path (rejected, minimized, cancelled,
        // absorb-and-drop, worker error), the derived host must re-arm its
        // full-refresh flag so the retry re-stages the world. The true value
        // clears it at settlement.
        bool m_full_refresh{false};
        // Consumption-ack manifest for the driver dirty-set covered by this
        // frame's staging payload: (seed, chunk ids, revisions) captured at the
        // capture point. The lane round-trips it into RenderFrameResult; the
        // derived owner consumes it there so the ack rides the reliable per-slot
        // settlement path (applied, not dropped, not errored) rather than staging.
        u64 m_ack_seed{};
        Array<u32> m_ack_chunk_ids{};
        Array<u64> m_ack_revisions{};
    };
}
