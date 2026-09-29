module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import std;

namespace pP::tests::detail {
    namespace GridTiles {
        [[nodiscard]] CameraSnapshot orthoSnapshot800x600() {
            Camera cam;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            cam.updateModel(std::chrono::milliseconds{16}, model,
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

            // GridFrame is uploaded as one 64-byte blob into g_grid_frame and
            // is not layout-adaptive: a single view-projection matrix.
            PPR_TEST_ASSERT(sizeof(GridPass::GridFrame) == 64u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GridPass::GridFrame, m_view_projection) == 0u);

            // Chunk-sized granularity: one tile per chunk, never a second
            // tiling system; the cache clamps at one entry per world chunk.
            PPR_TEST_ASSERT(GridPass::kTilesPerChunk == 1u);
            PPR_TEST_ASSERT(GridPass::kGridTileCacheCapacity == 1024u);
            PPR_TEST_ASSERT(GridPass::kCellsPerChunk == 16384u);
            PPR_TEST_ASSERT(GridPass::kCellWordsPerChunk == 8192u);
        };

        // Ortho 800x600 at the identity pose sees world x in [0, 800] and y
        // in [0, 600]: tile A sits inside, tile B is far off-screen, tile C
        // touches the x = 800 edge (visible per the boundary rule), and tile
        // D starts one cell past it (culled). The plan preserves submission
        // order over the visible set.
        PPR_UNIT_TEST (grid_plan_culls_offscreen_tiles) {
            const CameraSnapshot snapshot = orthoSnapshot800x600();

            const GridTileSubmission submissions[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
                {.m_chunk_id = 1u, .m_tile_range = {2000, 2000, 2128, 2128}, .m_dirty_mask = 1u, .m_material_id = 4u},
                {.m_chunk_id = 2u, .m_tile_range = {800, 0, 928, 128}, .m_dirty_mask = 0u, .m_material_id = 5u},
                {.m_chunk_id = 3u, .m_tile_range = {801, 0, 929, 128}, .m_dirty_mask = 0u, .m_material_id = 6u},
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
                {.m_chunk_id = 9u, .m_tile_range = {128, 0, 0, 128}, .m_dirty_mask = 1u},
            };
            const Expected<GridTilePlan> malformed_plan = GridPass::planTiles(snapshot, malformed);
            PPR_TEST_ASSERT(not malformed_plan.has_value());
            PPR_TEST_ASSERT(malformed_plan.error() == std::make_error_code(std::errc::invalid_argument));

            // Reserved dirty-mask bits name tiles that do not exist (chunk-sized
            // tiles use bit 0 only), so they fail closed as well.
            const GridTileSubmission reserved[] = {
                {.m_chunk_id = 7u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 2u},
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
                {.m_chunk_id = 7u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 2u},
                {.m_chunk_id = 9u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 1u, .m_material_id = 5u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            PPR_TEST_ASSERT(pass.stagedTileCount() == 2u);

            const GridTilePayload payload_a{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}, .m_material = 2u};
            const GridTilePayload views_a[] = {payload_a};
            const Expected<u32> uploaded_a = pass.requestUpload(
                {.m_chunk_id = 7u, .m_tile_data_view = views_a});
            PPR_TEST_ASSERT(uploaded_a.has_value());
            PPR_TEST_ASSERT(*uploaded_a == 1u);

            const GridTilePayload payload_b{.m_rect = {128.0f, 0.0f, 256.0f, 128.0f}, .m_material = 5u};
            const GridTilePayload views_b[] = {payload_b};
            const Expected<u32> uploaded_b = pass.requestUpload(
                {.m_chunk_id = 9u, .m_tile_data_view = views_b});
            PPR_TEST_ASSERT(uploaded_b.has_value());
            PPR_TEST_ASSERT(*uploaded_b == 1u);
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 2u);

            // The edit moves on: both chunks resubmit clean, so their uploads
            // must be no-ops while the cache keeps serving them.
            const GridTileSubmission quiescent[] = {
                {.m_chunk_id = 7u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 0u, .m_material_id = 2u},
                {.m_chunk_id = 9u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 0u, .m_material_id = 5u},
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
            PPR_TEST_ASSERT(pass.requestUpload({.m_chunk_id = 99u}).error() ==
                            std::make_error_code(std::errc::invalid_argument));
        };

        // Reject malformed optional cell views and mismatched chunk geometry
        // before changing the existing cached tile. Flat-only callers retain
        // the original freedom to stage arbitrary tile rectangles/chunk IDs.
        PPR_UNIT_TEST (grid_cell_upload_rejects_invalid_requests_without_mutation) {
            GridPass pass{};
            const GridTileSubmission staged[] = {
                {.m_chunk_id = 1u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 1u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            const GridTilePayload original{.m_rect = {128.0f, 0.0f, 256.0f, 128.0f}, .m_material = 3u};
            const GridTilePayload replacement{.m_rect = {128.0f, 0.0f, 256.0f, 128.0f}, .m_material = 9u};
            const GridTilePayload original_view[] = {original};
            const GridTilePayload replacement_view[] = {replacement};
            const Expected<u32> first = pass.requestUpload({.m_chunk_id = 1u, .m_tile_data_view = original_view});
            PPR_TEST_ASSERT(first.has_value() and * first == 1u);

            std::vector<u16> cells(GridPass::kCellsPerChunk, 0xffffu);
            const auto rejects = [&pass, &replacement_view](const u32 chunk_id, std::span<const u16> input) {
                const Expected<u32> result = pass.requestUpload({
                    .m_chunk_id = chunk_id, .m_tile_data_view = replacement_view, .m_cell_materials = input
                });
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
                {.m_chunk_id = 1u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u},
                {.m_chunk_id = 1024u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u},
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
            PPR_TEST_ASSERT(flat.has_value() and * flat == 1u);
            PPR_TEST_ASSERT(pass.resolveCached(1u)->m_material == 9u);

            const GridTileSubmission clean_wrong_range[] = {
                {.m_chunk_id = 1u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 0u},
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
                {.m_chunk_id = 33u, .m_tile_range = {128, 128, 256, 256}, .m_dirty_mask = 1u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(dirty));
            std::vector<u16> cells(GridPass::kCellsPerChunk, 0xffffu);
            cells[0] = 4u;
            cells[GridPass::kCellsPerChunk - 1u] = 7u;
            GridTilePayload payload{.m_rect = {128.0f, 128.0f, 256.0f, 256.0f}, .m_material = 4u};
            const Expected<u32> uploaded = pass.requestUpload({
                .m_chunk_id = 33u, .m_tile_data_view = std::span{&payload, 1u}, .m_cell_materials = cells
            });
            PPR_TEST_ASSERT(uploaded.has_value() and * uploaded == 1u);
            cells.clear(); // borrowed input must not be required after return
            payload.m_material = 99u;
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 4u);

            const GridTilePayload replacement{.m_rect = {128.0f, 128.0f, 256.0f, 256.0f}, .m_material = 8u};
            const GridTilePayload replacement_view[] = {replacement};
            const Expected<u32> replaced = pass.requestUpload({.m_chunk_id = 33u, .m_tile_data_view = replacement_view});
            PPR_TEST_ASSERT(replaced.has_value() and * replaced == 1u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 1u);
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 8u);

            const GridTileSubmission clean[] = {
                {.m_chunk_id = 33u, .m_tile_range = {128, 128, 256, 256}, .m_dirty_mask = 0u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(clean));
            cells.resize(GridPass::kCellsPerChunk, 12u);
            const Expected<u32> no_op = pass.requestUpload({.m_chunk_id = 33u, .m_cell_materials = cells});
            PPR_TEST_ASSERT(no_op.has_value() and * no_op == 0u);
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 8u);

            // Even though clean requests do not write, supplied spans must
            // still be validated before returning the zero-upload result.
            const Expected<u32> malformed_clean = pass.requestUpload({
                .m_chunk_id = 33u, .m_cell_materials = std::span<const u16>{cells.data(), 1u}
            });
            PPR_TEST_ASSERT(not malformed_clean.has_value());
            PPR_TEST_ASSERT(malformed_clean.error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(pass.resolveCached(33u)->m_material == 8u);
        };

        PPR_UNIT_TEST (grid_clear_cache_preserves_staging_but_clear_tiles_does_not_invalidate_cache) {
            GridPass pass{};
            const GridTileSubmission staged[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u},
            };
            const GridTilePayload payload{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}, .m_material = 5u};
            const GridTilePayload view[] = {payload};
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            std::vector<u16> cells(GridPass::kCellsPerChunk, 5u);
            const Expected<u32> initial = pass.requestUpload({
                .m_chunk_id = 0u, .m_tile_data_view = view, .m_cell_materials = cells
            });
            PPR_TEST_ASSERT(initial.has_value() and * initial == 1u);

            pass.clearTiles();
            PPR_TEST_ASSERT(pass.stagedTileCount() == 0u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 1u);
            PPR_TEST_ASSERT(pass.resolveCached(0u)->m_material == 5u);
            PPR_TEST_ASSERT(pass.requestUpload({.m_chunk_id = 0u, .m_tile_data_view = view}).error() ==
                            std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            pass.clearCache(); // regeneration: remove old cache but retain staged tiles
            PPR_TEST_ASSERT(pass.stagedTileCount() == 1u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == 0u);
            PPR_TEST_ASSERT(pass.resolveCached(0u).error() == std::make_error_code(std::errc::invalid_argument));
            const Expected<u32> regenerated = pass.requestUpload({.m_chunk_id = 0u, .m_tile_data_view = view});
            PPR_TEST_ASSERT(regenerated.has_value() and * regenerated == 1u);
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
                    staged.push_back(GridTileSubmission{
                        .m_chunk_id = chunk_y * 32u + chunk_x,
                        .m_tile_range = {
                            min_x, min_y,
                            min_x + static_cast<i32>(GridPass::kGridTileEdge),
                            min_y + static_cast<i32>(GridPass::kGridTileEdge)
                        },
                        .m_dirty_mask = 1u,
                    });
                }
            }
            PPR_TEST_ASSERT(not pass.submitTiles({staged.data(), staged.size()}));

            for (u32 chunk_id = 0u; chunk_id < GridPass::kGridTileCacheCapacity; ++chunk_id) {
                const GridTilePayload payload{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}};
                const GridTilePayload views[] = {payload};
                const Expected<u32> uploaded = pass.requestUpload(
                    {.m_chunk_id = chunk_id, .m_tile_data_view = views});
                PPR_TEST_ASSERT(uploaded.has_value());
                PPR_TEST_ASSERT(*uploaded == 1u);
            }
            PPR_TEST_ASSERT(pass.uploadCount() == GridPass::kGridTileCacheCapacity);
            PPR_TEST_ASSERT(pass.cachedTileCount() == GridPass::kGridTileCacheCapacity);

            // Overwriting a live entry reuses its slot: still exactly one tile.
            const GridTilePayload refill{.m_rect = {0.0f, 0.0f, 128.0f, 128.0f}};
            const GridTilePayload refill_views[] = {refill};
            const Expected<u32> overwrite = pass.requestUpload(
                {.m_chunk_id = 0u, .m_tile_data_view = refill_views});
            PPR_TEST_ASSERT(overwrite.has_value());
            PPR_TEST_ASSERT(*overwrite == 1u);
            PPR_TEST_ASSERT(pass.cachedTileCount() == GridPass::kGridTileCacheCapacity);

            // A 1025th distinct chunk has no slot left: fail closed.
            const GridTileSubmission overflow[] = {
                {
                    .m_chunk_id = GridPass::kGridTileCacheCapacity,
                    .m_tile_range = {0, 0, 128, 128},
                    .m_dirty_mask = 1u
                },
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
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
                {.m_chunk_id = 1u, .m_tile_range = {2000, 2000, 2128, 2128}, .m_dirty_mask = 1u, .m_material_id = 4u},
                {.m_chunk_id = 2u, .m_tile_range = {800, 0, 928, 128}, .m_dirty_mask = 0u, .m_material_id = 5u},
                {.m_chunk_id = 3u, .m_tile_range = {801, 0, 929, 128}, .m_dirty_mask = 0u, .m_material_id = 6u},
            };
            const Expected<GridTilePlan> plan = GridPass::planTiles(snapshot, submissions);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->m_tiles.size() == 2u);

            const Expected<Array<rhi::IndirectDrawArguments> > args = GridPass::compactIndirect(*plan);
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
            const Expected<Array<rhi::IndirectDrawArguments> > empty_args = GridPass::compactIndirect(empty_plan);
            PPR_TEST_ASSERT(empty_args.has_value());
            PPR_TEST_ASSERT(empty_args->empty());

            GridTilePlan full_plan{};
            full_plan.m_tiles.reserve(GridPass::kGridTileCacheCapacity);
            for (u32 index = 0u; index < GridPass::kGridTileCacheCapacity; ++index) {
                full_plan.m_tiles.push_back(GridPlannedTile{
                    .m_submission_index = index,
                    .m_chunk_id = index,
                });
            }
            const Expected<Array<rhi::IndirectDrawArguments> > full_args = GridPass::compactIndirect(full_plan);
            PPR_TEST_ASSERT(full_args.has_value());
            PPR_TEST_ASSERT(full_args->size() == GridPass::kGridTileCacheCapacity);
            PPR_TEST_ASSERT((*full_args)[0u].startInstanceLocation == 0u);
            PPR_TEST_ASSERT((*full_args)[GridPass::kGridTileCacheCapacity - 1u].startInstanceLocation ==
                            GridPass::kGridTileCacheCapacity - 1u);

            GridTilePlan over_plan{};
            over_plan.m_tiles.reserve(GridPass::kGridTileCacheCapacity + 1u);
            for (u32 index = 0u; index <= GridPass::kGridTileCacheCapacity; ++index) {
                over_plan.m_tiles.push_back(GridPlannedTile{
                    .m_submission_index = index,
                    .m_chunk_id = index,
                });
            }
            const Expected<Array<rhi::IndirectDrawArguments> > over_args = GridPass::compactIndirect(over_plan);
            PPR_TEST_ASSERT(not over_args.has_value());
            PPR_TEST_ASSERT(over_args.error() == std::make_error_code(std::errc::no_buffer_space));
        };

        // CPU/indirect agreement: both lanes derive from the same plan, so
        // the indirect lane covers the same visible set in the same order.
        // The CPU lane draws one instance per planned tile in plan order; the
        // indirect lane issues one single-instance arg per tile whose
        // startInstanceLocation is that tile's plan index. Staging runs
        // through the real submitTiles entry; everything stays device-free.
        PPR_UNIT_TEST (grid_cpu_and_indirect_lanes_agree_on_plan) {
            GridPass pass{};
            const GridTileSubmission staged[] = {
                {.m_chunk_id = 10u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 1u},
                {.m_chunk_id = 11u, .m_tile_range = {2000, 2000, 2128, 2128}, .m_dirty_mask = 1u, .m_material_id = 2u},
                {.m_chunk_id = 12u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 0u, .m_material_id = 3u},
                {.m_chunk_id = 13u, .m_tile_range = {3000, 3000, 3128, 3128}, .m_dirty_mask = 0u, .m_material_id = 4u},
            };
            PPR_TEST_ASSERT(not pass.submitTiles(staged));
            PPR_TEST_ASSERT(pass.stagedTileCount() == 4u);

            const CameraSnapshot snapshot = orthoSnapshot800x600();
            const Expected<GridTilePlan> plan = GridPass::planTiles(snapshot, staged);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->m_tiles.size() == 2u);
            PPR_TEST_ASSERT(plan->m_tiles[0].m_chunk_id == 10u);
            PPR_TEST_ASSERT(plan->m_tiles[1].m_chunk_id == 12u);

            const Expected<Array<rhi::IndirectDrawArguments> > args = GridPass::compactIndirect(*plan);
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
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest grid_pass = UnitTest::Named("grid_pass") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::GridTiles::grid_tile_payload_layout_matches_the_shader,
            detail::GridTiles::grid_plan_culls_offscreen_tiles,
            detail::GridTiles::grid_dirty_upload_serves_quiescent_from_cache,
            detail::GridTiles::grid_cell_upload_rejects_invalid_requests_without_mutation,
            detail::GridTiles::grid_cell_upload_replaces_dirty_and_clean_requests_are_no_ops,
            detail::GridTiles::grid_clear_cache_preserves_staging_but_clear_tiles_does_not_invalidate_cache,
            detail::GridTiles::grid_tile_cache_capacity_clamps,
            detail::GridTiles::grid_indirect_compaction_matches_visible_tiles,
            detail::GridTiles::grid_indirect_compaction_clamps_and_handles_empty,
            detail::GridTiles::grid_cpu_and_indirect_lanes_agree_on_plan,
        });
    };

    const UnitTest &grid_passTests() noexcept {
        return grid_pass;
    }
} // namespace pP::tests
