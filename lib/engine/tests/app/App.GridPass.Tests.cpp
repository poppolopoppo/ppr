module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.image;
import engine.math;
import engine.rhi;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // grid pass
    // ------------------------------------------------------------------

    namespace GridTiles {
        [[nodiscard]] CameraSnapshot orthoSnapshot800x600() {
            Camera cam;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            cam.updateModel(
                std::chrono::milliseconds{16}, model,
                Viewport{PixelRect{int2{0, 0}, int2{800, 600}}, ViewportLayout{}});
            return cam.getSnapshot();
        }

        // GPU payload layout, shared with grid_tiles.slang: 32 B = 16 rect +
        // four 4-byte fields, 16-aligned. A field-width or order change on
        // either side silently reinterprets every draw, so the contract is
        // pinned here rather than left to a draw that would look plausible.
        PPR_UNIT_TEST (grid_tile_payload_layout_matches_the_shader) {
            PPR_TEST_ASSERT(sizeof(GridTilePayload) == 32u);
            PPR_TEST_ASSERT(alignof(GridTilePayload) == 16u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTilePayload, m_rect) == 0u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTilePayload, m_material) == 16u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTilePayload, m_cell_word_base) == 20u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTilePayload, m_flags) == 24u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTilePayload, m_pad) == 28u);
            PPR_TEST_ASSERT(std::is_trivially_copyable_v<GridTilePayload>);
            PPR_TEST_ASSERT(std::is_standard_layout_v<GridTilePayload>);

            // GridFrame's matrix, visibility selector and metric share the shader ABI.
            PPR_TEST_ASSERT(sizeof(GridPass::GridFrame) == 336u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_view_projection) == 0u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_use_visible_ids) == 64u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_cell_size_m) == 68u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_surface_mode) == 72u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_boundary_material_id) == 76u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_inverse_view_projection) == 80u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_viewport) == 144u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_camera_origin) == 160u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_has_background) == 168u);
            // Offset 172 stays padding on both sides of the shader mirror.
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_layers) == 176u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_camera_world_x) == 304u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_camera_forward_x) == 316u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_camera_ortho) == 328u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_has_resolved_cells) == 332u);
            PPR_TEST_ASSERT(sizeof(GridBackgroundLayer) == 32u);
            PPR_TEST_ASSERT(sizeof(GridTerrainVisual) == 96u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTerrainVisual, m_filtered_mean_rgb) == 20u);
            PPR_TEST_ASSERT(sizeof(GridTerrainVisual::m_filtered_mean_rgb) == 12u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTerrainVisual, m_normal_texture_index) == 32u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTerrainVisual, m_metallic) == 36u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTerrainVisual, m_emissive_rgb) == 48u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTerrainVisual, m_surface_flags) == 76u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTerrainVisual, m_transport_model) == 80u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridTerrainVisual, m_sigma_per_m) == 84u);
            PPR_TEST_ASSERT(sizeof(math::ContourPoint) == 8u);
            PPR_TEST_ASSERT(sizeof(math::ContourTriangle) == 16u);
            PPR_TEST_ASSERT(sizeof(math::ContourCase) == 72u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(math::ContourCase, m_segment_count) == 0u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(math::ContourCase, m_triangle_count) == 4u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(math::ContourCase, m_segments) == 8u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(math::ContourCase, m_triangles) == 24u);

            // Chunk-sized granularity: one tile per chunk, never a second
            // tiling system; the cache clamps at one entry per world chunk.
            PPR_TEST_ASSERT(GridPass::kTilesPerChunk == 1u);
            PPR_TEST_ASSERT(GridPass::kGridTileCacheCapacity == 1024u);
            PPR_TEST_ASSERT(GridPass::kCellsPerChunk == 16384u);
            PPR_TEST_ASSERT(GridPass::kCellWordsPerChunk == 8192u);
        };

        // ------------------------------------------------------------------
        // scalar material validation
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (grid_material_validation_rejects_invalid_values) {
            GridTerrainVisual visual{};
            visual.m_emissive_rgb[0] = 0.5f;
            visual.m_emissive_rgb[1] = 0.25f;
            visual.m_emissive_rgb[2] = 0.125f;
            visual.m_emissive_strength = 4.0f;
            PPR_TEST_ASSERT(not validateGridMaterialOptics(visual));

            visual.m_transport_model = kGridTransportOpaqueV1;
            visual.m_surface_flags = kGridSurfaceOpticalBlend;
            visual.m_opacity = 0.25f;
            PPR_TEST_ASSERT(not validateGridMaterialOptics(visual));

            visual.m_transport_model = kGridTransportAbsorbingV1;
            visual.m_sigma_per_m[0] = 0.5f;
            visual.m_sigma_per_m[1] = 1.0f;
            PPR_TEST_ASSERT(not validateGridMaterialOptics(visual));

            // A vacuum row may not emit: emission has no surface to leave from.
            visual.m_transport_model = kGridTransportVacuumV1;
            PPR_TEST_ASSERT(validateGridMaterialOptics(visual) == std::errc::invalid_argument);
            std::ranges::fill(visual.m_sigma_per_m, 0.0f);
            visual.m_emissive_strength = 0.0f;
            PPR_TEST_ASSERT(not validateGridMaterialOptics(visual));

            visual.m_transport_model = 0x201u;
            PPR_TEST_ASSERT(validateGridMaterialOptics(visual) == std::errc::invalid_argument);
            visual.m_transport_model = kGridTransportAbsorbingV1;
            visual.m_sigma_per_m[0] = (std::numeric_limits<float>::max)();
            visual.m_emissive_strength = (std::numeric_limits<float>::max)();
            PPR_TEST_ASSERT(validateGridMaterialOptics(visual) == std::errc::invalid_argument);
        };

        // ------------------------------------------------------------------
        // coarse filtered albedo mean
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (grid_filtered_albedo_mean_accepts_full_and_truncated_chains) {
            // The image subsystem admits BOTH full chains through1x1 and
            // truncated chains through4x4. The coarse material mean consumes
            // the last actual level without imposing the truncated policy.
            image::ImageAsset base{};
            base.m_width = base.m_height = 8u;
            auto storage = mem::UniqueBuffer::allocate(8u * 8u * 4u);
            PPR_TEST_ASSERT(not storage.materialize());

            {
                const auto pixels = storage.getMutableData();
                PPR_TEST_ASSERT(pixels.has_value());

                for (std::size_t offset = 0u; offset < pixels->size(); offset += 4u) {
                    (*pixels)[offset] = std::byte{255};
                    (*pixels)[offset + 1u] = std::byte{128};
                    (*pixels)[offset + 2u] = std::byte{0};
                    (*pixels)[offset + 3u] = std::byte{255};
                }
            }

            PPR_TEST_ASSERT(not storage.moveToShared(&base.m_storage));
            base.m_subresources.push_back({.m_view = base.m_storage, .m_row_pitch = 32u, .m_slice_pitch = 256u});

            auto full = base;
            PPR_TEST_ASSERT(not image::generateMipChain(full, {.m_full_chain = true}));
            PPR_TEST_ASSERT(full.m_mip_count == 4u and full.m_subresources.size() == 4u);

            const auto full_mean = gridFilteredAlbedoMean(full);
            PPR_TEST_ASSERT(full_mean.has_value());
            PPR_TEST_ASSERT((*full_mean)[0] == 1.0f and (*full_mean)[2] == 0.0f);
            PPR_TEST_ASSERT(std::abs((*full_mean)[1] - 128.0f / 255.0f) <= 1e-6f);

            auto truncated = base;
            PPR_TEST_ASSERT(not image::generateMipChain(truncated, {}));
            PPR_TEST_ASSERT(truncated.m_mip_count == 2u and truncated.m_subresources.size() == 2u);

            const auto truncated_mean = gridFilteredAlbedoMean(truncated);
            PPR_TEST_ASSERT(truncated_mean.has_value() and *truncated_mean == *full_mean);

            auto overlong = full;
            ++overlong.m_mip_count;
            overlong.m_subresources.push_back(overlong.m_subresources.back());
            const auto rejected_overlong = gridFilteredAlbedoMean(overlong);
            PPR_TEST_ASSERT(not rejected_overlong and rejected_overlong.error() == std::errc::invalid_argument);

            auto missing_level = full;
            missing_level.m_subresources.pop_back();
            const auto rejected_missing = gridFilteredAlbedoMean(missing_level);
            PPR_TEST_ASSERT(not rejected_missing and rejected_missing.error() == std::errc::invalid_argument);
        };


        PPR_UNIT_TEST (grid_plan_converts_fine_bounds_to_metres) {
            const CameraSnapshot snapshot = orthoSnapshot800x600();
            const GridTileSubmission tiles[] = {
                {.m_tile_range = {2304, 0, 2432, 128}, .m_chunk_id = 0u},
                {.m_tile_range = {2432, 0, 2560, 128}, .m_chunk_id = 1u},
            };
            const auto legacy = GridPass::planTiles(snapshot, tiles);
            PPR_TEST_ASSERT(legacy and legacy->m_tiles.empty());
            const auto metric = GridPass::planTiles(snapshot, tiles, 1.0f / 3.0f);
            PPR_TEST_ASSERT(metric and metric->m_tiles.size() == 1u);
            PPR_TEST_ASSERT(metric->m_tiles[0u].m_chunk_id == 0u);
            for (const float invalid:
                {0.0f, -1.0f, (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::denorm_min)(), 0.5f,
                    std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                const auto rejected = GridPass::planTiles(snapshot, tiles, invalid);
                PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);
                GridPass pass{};
                GridPass::GridStagedFrame stage{};
                stage.m_cell_size_m = invalid;
                PPR_TEST_ASSERT(pass.applyStagedFrame(stage) == std::errc::invalid_argument);
            }
        };

        // Ortho 800x600 at the identity pose sees world x in [0, 800] and y
        // in [0, 600]: tile A sits inside, tile B is far off-screen, tile C
        // touches the x = 800 edge (visible per the boundary rule), and tile
        // D starts one cell past it (culled). The plan preserves submission
        // order over the visible set.
        PPR_UNIT_TEST (grid_plan_culls_offscreen_tiles) {
            const CameraSnapshot snapshot = orthoSnapshot800x600();

            const GridTileSubmission submissions[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u, .m_material_id = 3u},
                {.m_dirty_mask = 1u, .m_tile_range = {2000, 2000, 2128, 2128}, .m_chunk_id = 1u, .m_material_id = 4u},
                {.m_dirty_mask = 0u, .m_tile_range = {800, 0, 928, 128}, .m_chunk_id = 2u, .m_material_id = 5u},
                {.m_dirty_mask = 0u, .m_tile_range = {801, 0, 929, 128}, .m_chunk_id = 3u, .m_material_id = 6u},
            };
            const Expected<GridTilePlan> plan = GridPass::planTiles(snapshot, submissions);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->m_tiles.size() == 2u);
            PPR_TEST_ASSERT(plan->m_tiles[0].m_chunk_id == 0u);
            PPR_TEST_ASSERT(plan->m_tiles[0].m_submission_index == 0u);
            PPR_TEST_ASSERT(plan->m_tiles[0].m_material_id == 3u);
            PPR_TEST_ASSERT(plan->m_tiles[1].m_chunk_id == 2u);
            PPR_TEST_ASSERT(plan->m_tiles[1].m_submission_index == 2u);
            PPR_TEST_ASSERT(plan->m_tiles[1].m_material_id == 5u);

            // Degenerate inputs stay valid: an empty span plans no tiles.
            const Expected<GridTilePlan> empty_plan = GridPass::planTiles(
                snapshot, std::span<const GridTileSubmission>{});
            PPR_TEST_ASSERT(empty_plan.has_value());
            PPR_TEST_ASSERT(empty_plan->m_tiles.empty());

            // Malformed ranges fail closed instead of planning garbage.
            const GridTileSubmission malformed[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {128, 0, 0, 128}, .m_chunk_id = 9u},
            };
            const Expected<GridTilePlan> malformed_plan = GridPass::planTiles(snapshot, malformed);
            PPR_TEST_ASSERT(not malformed_plan.has_value());
            PPR_TEST_ASSERT(malformed_plan.error() == std::make_error_code(std::errc::invalid_argument));

            // Reserved dirty-mask bits name tiles that do not exist (chunk-sized
            // tiles use bit 0 only), so they fail closed as well.
            const GridTileSubmission reserved[] = {
                {.m_dirty_mask = 2u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 7u},
            };
            const Expected<GridTilePlan> reserved_plan = GridPass::planTiles(snapshot, reserved);
            PPR_TEST_ASSERT(not reserved_plan.has_value());
            PPR_TEST_ASSERT(reserved_plan.error() == std::make_error_code(std::errc::invalid_argument));
        };

        // Dirty-chunk upload lane: exactly the touched tiles upload (asserted
        // as counts, not pixels); quiescent chunks upload nothing and keep
        // serving from the cache. Staging and upload are pure CPU, so the
        // whole lane runs device-free; only render needs a device.
        PPR_UNIT_TEST (grid_dirty_upload_serves_quiescent_from_cache) {
            GridPass pass{};
            PPR_TEST_ASSERT(pass.uploadCount() == 0u);
            PPR_TEST_ASSERT(pass.cacheHitCount() == 0u);
            PPR_TEST_ASSERT(pass.stagedTileCount() == 0u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 0u);

            const GridTileSubmission staged[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 7u, .m_material_id = 2u},
                {.m_dirty_mask = 1u, .m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 9u, .m_material_id = 5u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            PPR_TEST_ASSERT(pass.stagedTileCount() == 2u);

            const GridTilePayload payload_a{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}, .m_material = 2u};
            const GridTilePayload views_a[] = {payload_a};
            const Expected<u32> uploaded_a = pass.requestUpload({.m_chunk_id = 7u, .m_tile_data_view = views_a});
            PPR_TEST_ASSERT(uploaded_a.has_value());
            PPR_TEST_ASSERT(*uploaded_a == 1u);

            const GridTilePayload payload_b{.m_rect = {128.0f, 0.0f, 256.0f, 128.0f}, .m_material = 5u};
            const GridTilePayload views_b[] = {payload_b};
            const Expected<u32> uploaded_b = pass.requestUpload({.m_chunk_id = 9u, .m_tile_data_view = views_b});
            PPR_TEST_ASSERT(uploaded_b.has_value());
            PPR_TEST_ASSERT(*uploaded_b == 1u);
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 2u);

            // The edit moves on: both chunks resubmit clean, so their uploads
            // must be no-ops while the cache keeps serving them.
            const GridTileSubmission quiescent[] = {
                {.m_dirty_mask = 0u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 7u, .m_material_id = 2u},
                {.m_dirty_mask = 0u, .m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 9u, .m_material_id = 5u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(quiescent));
            const Expected<u32> reuploaded_a = pass.requestUpload({.m_chunk_id = 7u});
            PPR_TEST_ASSERT(reuploaded_a.has_value());
            PPR_TEST_ASSERT(*reuploaded_a == 0u);
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);

            const Expected<GridTilePayload> cached_a = pass.resolveCached(7u);
            PPR_TEST_ASSERT(cached_a.has_value());
            PPR_TEST_ASSERT(cached_a->m_material == 2u);
            PPR_TEST_ASSERT(cached_a->m_rect[0] == 0.0f && cached_a->m_rect[2] == 128.0f);
            const Expected<GridTilePayload> cached_b = pass.resolveCached(9u);
            PPR_TEST_ASSERT(cached_b.has_value());
            PPR_TEST_ASSERT(cached_b->m_material == 5u);
            PPR_TEST_ASSERT(pass.cacheHitCount() == 2u);

            // A dirty restage without view data fails closed: the lane never
            // uploads a partial tile.
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            const Expected<u32> starved = pass.requestUpload({.m_chunk_id = 7u});
            PPR_TEST_ASSERT(not starved.has_value());
            PPR_TEST_ASSERT(starved.error() == std::make_error_code(std::errc::invalid_argument));

            // Unknown chunks fail closed: there is no cache entry to serve.
            PPR_TEST_ASSERT(pass.resolveCached(99u).error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(
                pass.requestUpload({.m_chunk_id = 99u}).error() == std::make_error_code(std::errc::invalid_argument));
        };

        // Reject malformed optional cell views and mismatched chunk geometry
        // before changing the existing cached tile. Flat-only callers retain
        // the original freedom to stage arbitrary tile rectangles/chunk IDs.
        PPR_UNIT_TEST (grid_cell_upload_rejects_invalid_requests_without_mutation) {
            GridPass pass{};
            const GridTileSubmission staged[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 1u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            const GridTilePayload original{.m_rect = {128.0f, 0.0f, 256.0f, 128.0f}, .m_material = 3u};
            const GridTilePayload replacement{.m_rect = {128.0f, 0.0f, 256.0f, 128.0f}, .m_material = 9u};
            const GridTilePayload original_view[] = {original};
            const GridTilePayload replacement_view[] = {replacement};
            const Expected<u32> first = pass.requestUpload({.m_chunk_id = 1u, .m_tile_data_view = original_view});
            PPR_TEST_ASSERT(first.has_value() and *first == 1u);

            std::vector<u16> cells(GridPass::kCellsPerChunk, 0xffffu);
            const auto rejects = [&pass, &replacement_view](const u32 chunk_id, std::span<const u16> input) {
                const Expected<u32> result = pass.requestUpload(
                    {.m_chunk_id = chunk_id, .m_tile_data_view = replacement_view, .m_cell_materials = input});
                PPR_TEST_ASSERT(not result.has_value());
                PPR_TEST_ASSERT(result.error() == std::make_error_code(std::errc::invalid_argument));
            };
            rejects(1u, std::span<const u16>{cells.data(), 1u});
            rejects(1u, std::span<const u16>{cells.data(), cells.size() - 1u});
            cells.push_back(7u);
            rejects(1u, cells);
            cells.pop_back();
            rejects(1024u, cells); // unknown/out-of-domain chunk

            const GridTileSubmission wrong_range[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 1u},
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 1024u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(wrong_range));
            rejects(1u, cells);
            rejects(1024u, cells);
            PPR_TEST_ASSERT(pass.uploadCount() == 1u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 1u);
            const Expected<GridTilePayload> retained = pass.resolveCached(1u);
            PPR_TEST_ASSERT(retained.has_value() and retained->m_material == 3u);

            // The same wrong range is still legal in the legacy flat lane.
            const Expected<u32> flat = pass.requestUpload({.m_chunk_id = 1u, .m_tile_data_view = replacement_view});
            PPR_TEST_ASSERT(flat.has_value() and *flat == 1u);
            PPR_TEST_ASSERT(pass.resolveCached(1u)->m_material == 9u);

            const GridTileSubmission clean_wrong_range[] = {
                {.m_dirty_mask = 0u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 1u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(clean_wrong_range));
            rejects(1u, cells); // valid-length cells still check geometry on a clean request
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
            PPR_TEST_ASSERT(pass.resolveCached(1u)->m_material == 9u);
        };

        // A valid 16384-cell view is accepted; the tile payload is observably
        // copied. The cell copy itself requires a GPU readback to verify.
        // A later clean request, including a
        // supplied valid cell span, does not replace the cached tile.
        PPR_UNIT_TEST (grid_cell_upload_replaces_dirty_and_clean_requests_are_no_ops) {
            GridPass pass{};
            const GridTileSubmission dirty[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {128, 128, 256, 256}, .m_chunk_id = 33u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(dirty));
            std::vector<u16> cells(GridPass::kCellsPerChunk, 0xffffu);
            cells[0] = 4u;
            cells[GridPass::kCellsPerChunk - 1u] = 7u;
            GridTilePayload payload{.m_rect = {128.0f, 128.0f, 256.0f, 256.0f}, .m_material = 4u};
            const Expected<u32> uploaded = pass.requestUpload(
                {.m_chunk_id = 33u, .m_tile_data_view = std::span{&payload, 1u}, .m_cell_materials = cells});
            PPR_TEST_ASSERT(uploaded.has_value() and *uploaded == 1u);
            cells.clear(); // borrowed input must not be required after return
            payload.m_material = 99u;
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 4u);

            const GridTilePayload replacement{.m_rect = {128.0f, 128.0f, 256.0f, 256.0f}, .m_material = 8u};
            const GridTilePayload replacement_view[] = {replacement};
            const Expected<u32> replaced = pass.requestUpload(
                {.m_chunk_id = 33u, .m_tile_data_view = replacement_view});
            PPR_TEST_ASSERT(replaced.has_value() and *replaced == 1u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 1u);
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 8u);

            const GridTileSubmission clean[] = {
                {.m_dirty_mask = 0u, .m_tile_range = {128, 128, 256, 256}, .m_chunk_id = 33u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(clean));
            cells.resize(GridPass::kCellsPerChunk, 12u);
            const Expected<u32> no_op = pass.requestUpload({.m_chunk_id = 33u, .m_cell_materials = cells});
            PPR_TEST_ASSERT(no_op.has_value() and *no_op == 0u);
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 8u);

            // Even though clean requests do not write, supplied spans must
            // still be validated before returning the zero-upload result.
            const Expected<u32> malformed_clean = pass.requestUpload(
                {.m_chunk_id = 33u, .m_cell_materials = std::span<const u16>{cells.data(), 1u}});
            PPR_TEST_ASSERT(not malformed_clean.has_value());
            PPR_TEST_ASSERT(malformed_clean.error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 8u);
        };

        PPR_UNIT_TEST (grid_clear_cache_preserves_staging_but_clear_tiles_does_not_invalidate_cache) {
            GridPass pass{};
            const GridTileSubmission staged[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u},
            };
            const GridTilePayload payload{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}, .m_material = 5u};
            const GridTilePayload view[] = {payload};
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            std::vector<u16> cells(GridPass::kCellsPerChunk, 5u);
            const Expected<u32> initial = pass.requestUpload(
                {.m_chunk_id = 0u, .m_tile_data_view = view, .m_cell_materials = cells});
            PPR_TEST_ASSERT(initial.has_value() and *initial == 1u);

            pass.clearTiles();
            PPR_TEST_ASSERT(pass.stagedTileCount() == 0u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 1u);
            PPR_TEST_ASSERT(pass.resolveCached(0u)->m_material == 5u);
            PPR_TEST_ASSERT(
                pass.requestUpload({.m_chunk_id = 0u, .m_tile_data_view = view}).error() ==
                std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            pass.clearCache(); // regeneration: remove old cache but retain staged tiles
            PPR_TEST_ASSERT(pass.stagedTileCount() == 1u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 0u);
            PPR_TEST_ASSERT(pass.resolveCached(0u).error() == std::make_error_code(std::errc::invalid_argument));
            const Expected<u32> regenerated = pass.requestUpload({.m_chunk_id = 0u, .m_tile_data_view = view});
            PPR_TEST_ASSERT(regenerated.has_value() and *regenerated == 1u);
            PPR_TEST_ASSERT(pass.resolveCached(0u)->m_material == 5u);
        };

        // The upload cache clamps at one entry per world chunk: filling all
        // 1024 chunk slots succeeds, re-uploading a live entry still succeeds
        // (overwrite, no new slot), and the 1025th distinct chunk fails
        // closed with no_buffer_space instead of a partial upload.
        PPR_UNIT_TEST (grid_tile_cache_capacity_clamps) {
            PPR_TEST_ASSERT(GridPass::kGridTileCacheCapacity == 32u * 32u);

            GridPass pass{};
            Array<GridTileSubmission> staged{};
            staged.reserve(GridPass::kGridTileCacheCapacity);
            for (u32 chunk_y = 0u; chunk_y < 32u; ++chunk_y) {
                for (u32 chunk_x = 0u; chunk_x < 32u; ++chunk_x) {
                    const i32 min_x = static_cast<i32>(chunk_x * GridPass::kGridTileEdge);
                    const i32 min_y = static_cast<i32>(chunk_y * GridPass::kGridTileEdge);
                    staged.push_back(
                        GridTileSubmission{
                            .m_dirty_mask = 1u,
                            .m_tile_range =
                                {min_x, min_y, min_x + static_cast<i32>(GridPass::kGridTileEdge),
                                    min_y + static_cast<i32>(GridPass::kGridTileEdge)},
                            .m_chunk_id = chunk_y * 32u + chunk_x,
                        });
                }
            }
            PPR_TEST_ASSERT(not pass.submitTiles({staged.data(), staged.size()}));

            for (u32 chunk_id = 0u; chunk_id < GridPass::kGridTileCacheCapacity; ++chunk_id) {
                const GridTilePayload payload{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}};
                const GridTilePayload views[] = {payload};
                const Expected<u32> uploaded = pass.requestUpload({.m_chunk_id = chunk_id, .m_tile_data_view = views});
                PPR_TEST_ASSERT(uploaded.has_value());
                PPR_TEST_ASSERT(*uploaded == 1u);
            }
            PPR_TEST_ASSERT(pass.uploadCount() == GridPass::kGridTileCacheCapacity);
            PPR_TEST_ASSERT(pass.cachedTileCount() == GridPass::kGridTileCacheCapacity);

            // Overwriting a live entry reuses its slot: still exactly one tile.
            const GridTilePayload refill{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}};
            const GridTilePayload refill_views[] = {refill};
            const Expected<u32> overwrite = pass.requestUpload({.m_chunk_id = 0u, .m_tile_data_view = refill_views});
            PPR_TEST_ASSERT(overwrite.has_value());
            PPR_TEST_ASSERT(*overwrite == 1u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == GridPass::kGridTileCacheCapacity);

            // A 1025th distinct chunk has no slot left: fail closed.
            const GridTileSubmission overflow[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = GridPass::kGridTileCacheCapacity},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(overflow));
            const Expected<u32> clamped = pass.requestUpload(
                {.m_chunk_id = GridPass::kGridTileCacheCapacity, .m_tile_data_view = refill_views});
            PPR_TEST_ASSERT(not clamped.has_value());
            PPR_TEST_ASSERT(clamped.error() == std::make_error_code(std::errc::no_buffer_space));
            PPR_TEST_ASSERT(pass.cachedTileCount() == GridPass::kGridTileCacheCapacity);
        };

        // Indirect compaction exactness: one arg per visible tile in plan
        // order, each carrying its tile's first-payload offset. The layout
        // mirrors the ortho cull test above: chunk 1 is far off-screen and
        // chunk 3 starts one cell past the x = 800 edge, so the visible set
        // is chunks {0, 2} with compacted offsets {0, 1}. Pure CPU like
        // planTiles: no device, no GPU readback.
        PPR_UNIT_TEST (grid_indirect_compaction_matches_visible_tiles) {
            PPR_TEST_ASSERT(sizeof(rhi::IndirectDrawArguments) == 16u);

            const CameraSnapshot snapshot = orthoSnapshot800x600();

            const GridTileSubmission submissions[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u, .m_material_id = 3u},
                {.m_dirty_mask = 1u, .m_tile_range = {2000, 2000, 2128, 2128}, .m_chunk_id = 1u, .m_material_id = 4u},
                {.m_dirty_mask = 0u, .m_tile_range = {800, 0, 928, 128}, .m_chunk_id = 2u, .m_material_id = 5u},
                {.m_dirty_mask = 0u, .m_tile_range = {801, 0, 929, 128}, .m_chunk_id = 3u, .m_material_id = 6u},
            };
            const Expected<GridTilePlan> plan = GridPass::planTiles(snapshot, submissions);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->m_tiles.size() == 2u);

            const Expected<Array<rhi::IndirectDrawArguments>> args = GridPass::compactIndirect(*plan);
            PPR_TEST_ASSERT(args.has_value());
            PPR_TEST_ASSERT(args->size() == 2u);

            PPR_TEST_ASSERT((*args)[0].vertexCountPerInstance == GridPass::kGridTileVertexCount);
            PPR_TEST_ASSERT((*args)[0].instanceCount == 1u);
            PPR_TEST_ASSERT((*args)[0].startVertexLocation == 0u);
            PPR_TEST_ASSERT((*args)[0].startInstanceLocation == 0u);

            PPR_TEST_ASSERT((*args)[1].vertexCountPerInstance == GridPass::kGridTileVertexCount);
            PPR_TEST_ASSERT((*args)[1].instanceCount == 1u);
            PPR_TEST_ASSERT((*args)[1].startVertexLocation == 0u);
            PPR_TEST_ASSERT((*args)[1].startInstanceLocation == 1u);
        };

        // Compaction capacity clamp + empty-set behavior: an empty plan
        // compacts to zero args (zero tiles → zero draws, and the encode
        // returns before any pass call), a full-budget plan compacts whole,
        // and one tile past the budget fails closed with no_buffer_space —
        // the same policy as the tile-cache upload lane.
        PPR_UNIT_TEST (grid_indirect_compaction_clamps_and_handles_empty) {
            const GridTilePlan empty_plan{};
            const Expected<Array<rhi::IndirectDrawArguments>> empty_args = GridPass::compactIndirect(empty_plan);
            PPR_TEST_ASSERT(empty_args.has_value());
            PPR_TEST_ASSERT(empty_args->empty());

            GridTilePlan full_plan{};
            full_plan.m_tiles.reserve(GridPass::kGridTileCacheCapacity);
            for (u32 index = 0u; index < GridPass::kGridTileCacheCapacity; ++index) {
                full_plan.m_tiles.push_back(
                    GridPlannedTile{
                        .m_submission_index = index,
                        .m_chunk_id = index,
                    });
            }
            const Expected<Array<rhi::IndirectDrawArguments>> full_args = GridPass::compactIndirect(full_plan);
            PPR_TEST_ASSERT(full_args.has_value());
            PPR_TEST_ASSERT(full_args->size() == GridPass::kGridTileCacheCapacity);
            PPR_TEST_ASSERT((*full_args)[0u].startInstanceLocation == 0u);
            PPR_TEST_ASSERT(
                (*full_args)[GridPass::kGridTileCacheCapacity - 1u].startInstanceLocation ==
                GridPass::kGridTileCacheCapacity - 1u);

            GridTilePlan over_plan{};
            over_plan.m_tiles.reserve(GridPass::kGridTileCacheCapacity + 1u);
            for (u32 index = 0u; index <= GridPass::kGridTileCacheCapacity; ++index) {
                over_plan.m_tiles.push_back(
                    GridPlannedTile{
                        .m_submission_index = index,
                        .m_chunk_id = index,
                    });
            }
            const Expected<Array<rhi::IndirectDrawArguments>> over_args = GridPass::compactIndirect(over_plan);
            PPR_TEST_ASSERT(not over_args.has_value());
            PPR_TEST_ASSERT(over_args.error() == std::make_error_code(std::errc::no_buffer_space));
        };

        // CPU/indirect agreement: both lanes derive from the same plan, so
        // the indirect lane covers the same visible set in the same order.
        // The CPU lane draws one instance per planned tile in plan order; the
        // indirect lane issues one single-instance arg per tile whose
        // startInstanceLocation is that tile's plan index. Staging runs
        // through the real submitTiles entry; everything stays device-free.
        PPR_UNIT_TEST (grid_particle_rejection_preserves_the_retained_stage) {
            GridPass pass{};
            GridPass::GridStagedFrame original{};
            original.m_has_catalog = true;
            original.m_terrain_visuals.push_back(GridTerrainVisual{.m_material_id = 1u});
            original.m_tiles.push_back(
                GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridPass::GridStagedUpload upload{};
            upload.m_payload.m_material = 1u;
            upload.m_payload.m_rect[2] = 128.0f;
            upload.m_payload.m_rect[3] = 128.0f;
            original.m_uploads.push_back(upload);
            PPR_TEST_ASSERT(not pass.applyStagedFrame(original));
            const u64 uploads = pass.uploadCount();

            GridParticleInstance particle{};
            particle.m_id = 1u;
            particle.m_position_x_m = 32.01;
            particle.m_position_y_m = 32.99;
            particle.m_volume_m3 = 1.0;
            particle.m_mass_kg = 1.0;
            particle.m_temperature_k = 300.0;
            particle.m_material_id = 1u;
            const auto rejects = [&](const Array<GridParticleInstance> &records) {
                auto rejected = original;
                rejected.m_reset_caches = true;
                rejected.m_uploads[0].m_payload.m_material = 9u;
                rejected.m_particle_batches.push_back(
                    GridParticleBatch{0u, std::make_shared<const Array<GridParticleInstance>>(records)});
                PPR_TEST_ASSERT(pass.applyStagedFrame(rejected) == std::make_error_code(std::errc::invalid_argument));
                PPR_TEST_ASSERT(pass.uploadCount() == uploads);
                PPR_TEST_ASSERT(pass.cachedTileCount() == 1u and pass.stagedTileCount() == 1u);
                const auto retained = pass.resolveCached(0u);
                PPR_TEST_ASSERT(retained.has_value() and retained->m_material == 1u);
            };
            auto invalid = particle;
            invalid.m_volume_m3 = 0.0;
            rejects(Array<GridParticleInstance>{invalid});
            invalid = particle;
            invalid.m_position_x_m = 128.0;
            rejects(Array<GridParticleInstance>{invalid});
            invalid = particle;
            invalid.m_temperature_k = (std::numeric_limits<double>::quiet_NaN)();
            rejects(Array<GridParticleInstance>{invalid});
            invalid = particle;
            invalid.m_material_id = 2u;
            rejects(Array<GridParticleInstance>{invalid});

            auto accepted = original;
            accepted.m_tiles[0].m_dirty_mask = 0u;
            accepted.m_uploads.clear();
            accepted.m_particle_batches.push_back(
                GridParticleBatch{
                    0u, std::make_shared<const Array<GridParticleInstance>>(Array<GridParticleInstance>{particle})});
            PPR_TEST_ASSERT(not pass.applyStagedFrame(accepted));

            auto coincident = accepted;
            auto overlapping = particle;
            overlapping.m_id = 2u;
            coincident.m_particle_batches[0].m_particles = std::make_shared<const Array<GridParticleInstance>>(
                Array<GridParticleInstance>{particle, overlapping});
            PPR_TEST_ASSERT(not pass.applyStagedFrame(coincident));

            auto changed_thickness = accepted;
            changed_thickness.m_reference_thickness_m = 2.0f;
            PPR_TEST_ASSERT(
                pass.applyStagedFrame(changed_thickness) == std::make_error_code(std::errc::invalid_argument));
            auto changed_optics = accepted;
            changed_optics.m_terrain_visuals[0].m_surface_flags = 4u;
            PPR_TEST_ASSERT(pass.applyStagedFrame(changed_optics) == std::make_error_code(std::errc::invalid_argument));
            changed_optics.m_terrain_visuals[0].m_surface_flags = kGridSurfaceTextureRepeat;
            PPR_TEST_ASSERT(pass.applyStagedFrame(changed_optics) == std::make_error_code(std::errc::invalid_argument));
            changed_optics = accepted;
            changed_optics.m_terrain_visuals[0].m_ior = 1.5f;
            PPR_TEST_ASSERT(pass.applyStagedFrame(changed_optics) == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(not pass.applyStagedFrame(accepted));
            PPR_TEST_ASSERT(pass.uploadCount() == uploads);
        };

        PPR_UNIT_TEST (grid_moving_particles_allow_dense_chunks_and_closed_endpoints) {
            for (const double cell: {1.0, 1.0 / 3.0}) {
                GridPass pass{};
                GridPass::GridStagedFrame stage{};
                stage.m_cell_size_m = static_cast<float>(cell);
                stage.m_has_catalog = true;
                stage.m_terrain_visuals.push_back(GridTerrainVisual{.m_material_id = 1u});
                Array<GridParticleInstance> dense;
                dense.reserve(GridPass::kCellsPerChunk + 1u);
                for (u32 index = 0u; index <= GridPass::kCellsPerChunk; ++index) {
                    dense.push_back({index + 1u, 10.5 * cell, 10.5 * cell, cell * cell, 1.0, 300.0, 1u});
                }

                stage.m_particle_batches.push_back(
                    {0u, std::make_shared<const Array<GridParticleInstance>>(std::move(dense))});
                PPR_TEST_ASSERT(not pass.applyStagedFrame(stage));

                GridParticleInstance endpoint{
                    GridPass::kCellsPerChunk + 2u, 4096.0 * cell, 4096.0 * cell, cell * cell, 1.0, 300.0, 1u};
                stage.m_particle_batches.clear();
                stage.m_particle_batches.push_back(
                    {1023u,
                        std::make_shared<const Array<GridParticleInstance>>(Array<GridParticleInstance>{endpoint})});
                PPR_TEST_ASSERT(not pass.applyStagedFrame(stage));

                // The coordinate stays exact while only its owner bin is bounded.
                PPR_TEST_ASSERT((*stage.m_particle_batches[0u].m_particles)[0u].m_position_x_m == 4096.0 * cell);
                stage.m_particle_batches[0u].m_chunk_id = 1022u;
                PPR_TEST_ASSERT(pass.applyStagedFrame(stage) == std::errc::invalid_argument);
            }
        };

        PPR_UNIT_TEST (grid_cpu_and_indirect_lanes_agree_on_plan) {
            GridPass pass{};
            const GridTileSubmission staged[] = {
                {.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 10u, .m_material_id = 1u},
                {.m_dirty_mask = 1u, .m_tile_range = {2000, 2000, 2128, 2128}, .m_chunk_id = 11u, .m_material_id = 2u},
                {.m_dirty_mask = 0u, .m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 12u, .m_material_id = 3u},
                {.m_dirty_mask = 0u, .m_tile_range = {3000, 3000, 3128, 3128}, .m_chunk_id = 13u, .m_material_id = 4u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            PPR_TEST_ASSERT(pass.stagedTileCount() == 4u);

            const CameraSnapshot snapshot = orthoSnapshot800x600();
            const Expected<GridTilePlan> plan = GridPass::planTiles(snapshot, staged);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->m_tiles.size() == 2u);
            PPR_TEST_ASSERT(plan->m_tiles[0].m_chunk_id == 10u);
            PPR_TEST_ASSERT(plan->m_tiles[1].m_chunk_id == 12u);

            const Expected<Array<rhi::IndirectDrawArguments>> args = GridPass::compactIndirect(*plan);
            PPR_TEST_ASSERT(args.has_value());
            PPR_TEST_ASSERT(args->size() == plan->m_tiles.size());

            // Same visible set, same order, same total instance coverage.
            u32 covered_instances = 0u;
            const u32 arg_count = safe_narrowing(args->size());
            for (u32 index = 0u; index < arg_count; ++index) {
                PPR_TEST_ASSERT((*args)[index].instanceCount == 1u);
                PPR_TEST_ASSERT((*args)[index].startInstanceLocation == index);
                covered_instances += (*args)[index].instanceCount;
            }
            PPR_TEST_ASSERT(covered_instances == plan->m_tiles.size());
        };

        // ------------------------------------------------------------------
        // particle conic record
        // ------------------------------------------------------------------
        // CPU mirror of the shader's six-byte conic draw record
        // (grid_particle_native.slang particleConicBits / particleConicMetric and
        // the pair-packed layout). It pins the arithmetic, the packing and the
        // decoded support bound without a device; the GPU path is probed against
        // the same contract in the material leaf.

        // IEEE binary16 encode with round-to-nearest, ties-to-even: the rounding
        // the GPU f32tof16 conversion performs.
        [[nodiscard]] u16 conicHalfBits(const float value) {
            const u32 bits = std::bit_cast<u32>(value);
            const u32 sign = (bits >> 16u) & 0x8000u;
            const u32 magnitude = bits & 0x7fffffffu;
            if (magnitude > 0x7f800000u) {
                return static_cast<u16>(sign | 0x7e00u);
            }

            if (magnitude == 0x7f800000u) {
                return static_cast<u16>(sign | 0x7c00u);
            }

            if (magnitude == 0u) {
                return static_cast<u16>(sign);
            }

            const u32 exponent = magnitude >> 23u;
            u32 mantissa = magnitude & 0x7fffffu;
            const i32 half_exponent = static_cast<i32>(exponent) - 127 + 15;
            if (half_exponent >= 0x1f) {
                return static_cast<u16>(sign | 0x7c00u);
            }

            if (half_exponent <= 0) {
                // Subnormal half: the implicit-one mantissa on the 2^-24 unit
                // grid, with the same ties-to-even rounding.
                mantissa |= 0x800000u;
                const u32 shift = static_cast<u32>(14 - half_exponent);
                const u32 halfway = 1u << (shift - 1u);
                const u32 remainder = mantissa & ((1u << shift) - 1u);
                u32 result = mantissa >> shift;
                if (remainder > halfway or (remainder == halfway and (result & 1u) != 0u)) {
                    ++result;
                }

                return static_cast<u16>(sign | result);
            }

            const u32 rounded = (mantissa + 0xfffu + ((mantissa >> 13u) & 1u)) >> 13u;
            u32 next_exponent = static_cast<u32>(half_exponent);
            u32 half_mantissa = rounded;
            if (half_mantissa == 0x400u) {
                half_mantissa = 0u;
                ++next_exponent;
            }

            if (next_exponent >= 0x1f) {
                return static_cast<u16>(sign | 0x7c00u);
            }

            return static_cast<u16>(sign | (next_exponent << 10u) | half_mantissa);
        }

        // IEEE binary16 decode: exact, so a round trip isolates the encode.
        [[nodiscard]] float conicHalfValue(const u16 bits) {
            constexpr float subnormal_unit = std::bit_cast<float>(0x33800000u); // 2^-24
            const u32 sign = (bits & 0x8000u) != 0u ? 0x80000000u : 0u;
            const u32 exponent = (bits >> 10u) & 0x1fu;
            const u32 mantissa = bits & 0x3ffu;
            if (exponent == 0u) {
                const float magnitude = static_cast<float>(mantissa) * subnormal_unit;
                return std::bit_cast<float>(sign) * magnitude;
            }

            if (exponent == 0x1fu) {
                return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13u));
            }

            return std::bit_cast<float>(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
        }

        // The retained cue mapping and the record's coefficients in FP32, exactly
        // as grid_particle_native.slang particleConicBits derives them.
        struct ConicCoefficients {
            float m_a{}, m_b{}, m_c{};
            bool m_neutral{};
        };

        [[nodiscard]] ConicCoefficients conicCoefficients(const u32 angle_bias_stretch) {
            ConicCoefficients result{.m_a = 1.0f, .m_b = 0.0f, .m_c = 1.0f, .m_neutral = true};
            const u32 angle = angle_bias_stretch & 0xffffu;
            const u32 bias = (angle_bias_stretch >> 16u) & 0xffu;
            const u32 stretch = (angle_bias_stretch >> 24u) & 0xffu;
            if (bias == 0u and stretch == 0u) {
                return result;
            }

            constexpr float pi = 3.141592653589793f;
            const float radians = static_cast<float>(angle) * pi / 65536.0f;
            const float cosine = std::cos(radians);
            const float sine = std::sin(radians);
            const float anisotropy = kGridParticleTangentContourBias * (static_cast<float>(bias) / 255.0f);
            const float stretch_scale = 1.0f +
                                        (kGridParticleStretchCap - 1.0f) * (static_cast<float>(stretch) / 255.0f);
            const float along = 1.0f / (stretch_scale * stretch_scale);
            const float across_ratio = (1.0f + 0.5f * anisotropy) / (1.0f - 0.5f * anisotropy);
            const float across = across_ratio * across_ratio;

            result.m_a = cosine * cosine * along + sine * sine * across;
            result.m_b = cosine * sine * (along - across);
            result.m_c = sine * sine * along + cosine * cosine * across;
            result.m_neutral = false;
            return result;
        }

        [[nodiscard]] std::array<u16, 3u> conicBits(const u32 angle_bias_stretch) {
            const ConicCoefficients coefficients = conicCoefficients(angle_bias_stretch);
            return {conicHalfBits(coefficients.m_a), conicHalfBits(coefficients.m_b), conicHalfBits(coefficients.m_c)};
        }

        // Decoded, eigenvalue-corrected metric exactly as the vertex computes it.
        struct ConicMetric {
            float m_a{}, m_b{}, m_c{}, m_sqrt_determinant{}, m_support{};
        };

        [[nodiscard]] ConicMetric conicMetric(const std::array<u16, 3u> &bits) {
            ConicMetric result{};
            float a = conicHalfValue(bits[0u]);
            const float b = conicHalfValue(bits[1u]);
            float c = conicHalfValue(bits[2u]);
            const float difference = a - c;
            const float lambda_min = 0.5f * (a + c - std::sqrt(difference * difference + 4.0f * b * b));
            const float correction = std::max(0.0f, kGridParticleConicLambdaFloor - lambda_min);
            a += correction;
            c += correction;
            result.m_a = a;
            result.m_b = b;
            result.m_c = c;
            result.m_sqrt_determinant = std::sqrt(a * c - b * b);
            result.m_support = 1.0f / std::sqrt(std::max(lambda_min, kGridParticleConicLambdaFloor));
            return result;
        }

        // Pair-packed slot words: 12 bytes per two rows, three uints per pair.
        [[nodiscard]] Array<u32> packConicSlot(const std::span<const u32> words) {
            Array<u32> packed((words.size() + 1u) / 2u * 3u, 0u);
            for (std::size_t row = 0u; row < words.size(); row += 2u) {
                const std::array<u16, 3u> even = conicBits(words[row]);
                const std::array<u16, 3u> odd = row + 1u < words.size() ? conicBits(words[row + 1u])
                                                                        : std::array<u16, 3u>{0x3c00u, 0u, 0x3c00u};
                const std::size_t pair = row / 2u * 3u;
                packed[pair + 0u] = static_cast<u32>(even[0u]) | (static_cast<u32>(even[1u]) << 16u);
                packed[pair + 1u] = static_cast<u32>(even[2u]) | (static_cast<u32>(odd[0u]) << 16u);
                packed[pair + 2u] = static_cast<u32>(odd[1u]) | (static_cast<u32>(odd[2u]) << 16u);
            }

            return packed;
        }

        // The decoder's two aligned loads; the row parity selects the halfword.
        [[nodiscard]] std::array<u16, 3u> unpackConicRow(const std::span<const u32> packed, const std::size_t row) {
            const std::size_t base = 3u * (row / 2u) + row % 2u;
            const u32 low = packed[base];
            const u32 high = packed[base + 1u];
            if (row % 2u == 0u) {
                return {
                    static_cast<u16>(low & 0xffffu), static_cast<u16>(low >> 16u), static_cast<u16>(high & 0xffffu)};
            }

            return {static_cast<u16>(low >> 16u), static_cast<u16>(high & 0xffffu), static_cast<u16>(high >> 16u)};
        }

        // Admitted cue extremes plus rotated cases; a word is angle16 | B << 16 |
        // S << 24, and angle 0 with B = S = 0 is the canonical neutral.
        [[nodiscard]] Array<u32> conicProbeWords() {
            constexpr std::array<u32, 8u> angles{0u, 1u, 23170u, 32768u, 45000u, 49152u, 65535u, 12345u};
            constexpr std::array<u32, 6u> biases{0u, 1u, 64u, 128u, 200u, 255u};
            constexpr std::array<u32, 5u> stretches{0u, 1u, 32u, 128u, 255u};
            Array<u32> words{};
            words.push_back(0u);
            for (const u32 angle: angles) {
                for (const u32 bias: biases) {
                    for (const u32 stretch: stretches) {
                        words.push_back(angle | (bias << 16u) | (stretch << 24u));
                    }
                }
            }

            return words;
        }

        PPR_UNIT_TEST (grid_particle_conic_quantisation_and_support_bound) {
            // The record is exact at neutral, within the design's half-rounding
            // bounds per component, and its decoded support never reaches the
            // 3 r cull boundary.
            const Array<u32> words = conicProbeWords();
            float worst_a{};
            float worst_b{};
            float worst_c{};
            float worst_support{};
            float worst_radius{};
            for (const u32 word: words) {
                const ConicCoefficients exact = conicCoefficients(word);
                const std::array<u16, 3u> bits = conicBits(word);
                if (exact.m_neutral) {
                    PPR_TEST_ASSERT(bits[0u] == 0x3c00u and bits[1u] == 0u and bits[2u] == 0x3c00u);
                }

                const ConicMetric metric = conicMetric(bits);
                worst_a = std::max(worst_a, std::abs(metric.m_a - exact.m_a));
                worst_b = std::max(worst_b, std::abs(metric.m_b - exact.m_b));
                worst_c = std::max(worst_c, std::abs(metric.m_c - exact.m_c));
                worst_support = std::max(worst_support, metric.m_support);
                PPR_TEST_ASSERT(metric.m_sqrt_determinant > 0.0f);

                // Directional radius of the decoded ellipse against the exact one,
                // both in units of R: rho(u) = 1/sqrt(u^T M u).
                for (u32 step = 0u; step < 64u; ++step) {
                    const float angle = static_cast<float>(step) * 2.0f * std::numbers::pi_v<float> / 64.0f;
                    const float x = std::cos(angle);
                    const float y = std::sin(angle);
                    const float exact_quadratic = exact.m_a * x * x + 2.0f * exact.m_b * x * y + exact.m_c * y * y;
                    const float decoded_quadratic = metric.m_a * x * x + 2.0f * metric.m_b * x * y + metric.m_c * y * y;
                    PPR_TEST_ASSERT(exact_quadratic > 0.0f and decoded_quadratic > 0.0f);
                    const float radius = std::sqrt(exact_quadratic / decoded_quadratic) - 1.0f;
                    worst_radius = std::max(worst_radius, std::abs(radius));
                }
            }

            // Conservative half-rounding bounds of the design: |da|, |dc| <= 2^-10
            // and |db| <= 2^-12. The correction only ever raises the diagonal, so
            // it cannot exceed the decode error.
            PPR_TEST_ASSERT(worst_a <= 1.0f / 1024.0f + 1.0f / 262144.0f);
            PPR_TEST_ASSERT(worst_c <= 1.0f / 1024.0f + 1.0f / 262144.0f);
            PPR_TEST_ASSERT(worst_b <= 1.0f / 4096.0f);
            // Support semiaxis in units of R: 1.5 R = 3 r exactly at the k cap, and
            // the decoded side stays inside it.
            PPR_TEST_ASSERT(worst_support <= kGridParticleStretchCap);
            PPR_TEST_ASSERT(2.0f * worst_support <= kGridParticleCullRadiusScale);
            // The design's metric envelope is under 0.28 % in radius.
            PPR_TEST_ASSERT(worst_radius <= 0.0028f);

            _.log(
                std::format(
                    "GRID_CONIC quantise a={:.3e} b={:.3e} c={:.3e} support_R={:.9f} radius_error={:.3e} words={}",
                    worst_a, worst_b, worst_c, worst_support, worst_radius, words.size()));
        };

        PPR_UNIT_TEST (grid_particle_conic_pair_packing_round_trips_every_row) {
            // Odd/even/group-tail ABI: two aligned uint loads recover exactly the
            // record that was packed, including an odd final row whose partner is
            // the neutral record.
            for (const std::size_t count: {1u, 2u, 3u, 7u, 256u, 257u}) {
                Array<u32> words{};
                words.reserve(count);
                for (std::size_t row = 0u; row < count; ++row) {
                    words.push_back(
                        ((static_cast<u32>(row) * 7919u) & 0x00ff'ffffu) |
                        (((static_cast<u32>(row) * 37u) % 256u) << 16u) |
                        (((static_cast<u32>(row) * 53u) % 256u) << 24u));
                }

                const Array<u32> packed = packConicSlot(words);
                PPR_TEST_ASSERT(packed.size() == (count + 1u) / 2u * 3u);
                PPR_TEST_ASSERT(packed.size() * sizeof(u32) == gridParticleConicBytes(static_cast<u32>(count)));
                for (std::size_t row = 0u; row < count; ++row) {
                    PPR_TEST_ASSERT(unpackConicRow(packed, row) == conicBits(words[row]));
                }
            }
        };

        PPR_UNIT_TEST (grid_particle_conic_slot_sizing_matches_the_peer_review) {
            // 12 * ceil(capacity / 2) bytes per prepared slot; three slots are the
            // peer review's 18 MiB at the admitted 1,048,576-row capacity.
            PPR_TEST_ASSERT(gridParticleConicBytes(1u) == 12u);
            PPR_TEST_ASSERT(gridParticleConicBytes(2u) == 12u);
            PPR_TEST_ASSERT(gridParticleConicBytes(3u) == 24u);
            PPR_TEST_ASSERT(gridParticleConicBytes(4096u) == 24u * 1024u);
            PPR_TEST_ASSERT(gridParticleConicBytes(65'536u) == 384u * 1024u);
            PPR_TEST_ASSERT(gridParticleConicBytes(1'048'576u) == 6u * 1024u * 1024u);
            PPR_TEST_ASSERT(3u * gridParticleConicBytes(1'048'576u) == 18u * 1024u * 1024u);
            // The logical record stays six bytes: approximately 6 N.
            PPR_TEST_ASSERT(gridParticleConicBytes(1'048'576u) == 6u * 1'048'576u);
        };
    }

} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest grid_pass = UnitTest::Named("grid_pass") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::GridTiles::grid_tile_payload_layout_matches_the_shader,
            detail::GridTiles::grid_material_validation_rejects_invalid_values,
            detail::GridTiles::grid_filtered_albedo_mean_accepts_full_and_truncated_chains,
            detail::GridTiles::grid_plan_converts_fine_bounds_to_metres,
            detail::GridTiles::grid_plan_culls_offscreen_tiles,
            detail::GridTiles::grid_dirty_upload_serves_quiescent_from_cache,
            detail::GridTiles::grid_cell_upload_rejects_invalid_requests_without_mutation,
            detail::GridTiles::grid_cell_upload_replaces_dirty_and_clean_requests_are_no_ops,
            detail::GridTiles::grid_clear_cache_preserves_staging_but_clear_tiles_does_not_invalidate_cache,
            detail::GridTiles::grid_tile_cache_capacity_clamps,
            detail::GridTiles::grid_indirect_compaction_matches_visible_tiles,
            detail::GridTiles::grid_indirect_compaction_clamps_and_handles_empty,
            detail::GridTiles::grid_cpu_and_indirect_lanes_agree_on_plan,
            detail::GridTiles::grid_particle_rejection_preserves_the_retained_stage,
            detail::GridTiles::grid_moving_particles_allow_dense_chunks_and_closed_endpoints,
            detail::GridTiles::grid_particle_conic_quantisation_and_support_bound,
            detail::GridTiles::grid_particle_conic_pair_packing_round_trips_every_row,
            detail::GridTiles::grid_particle_conic_slot_sizing_matches_the_peer_review,
        });
    };

    const UnitTest &grid_passTests() noexcept {
        return grid_pass;
    }
} // namespace pP::tests
