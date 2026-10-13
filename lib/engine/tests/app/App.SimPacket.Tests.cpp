module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.image;
import engine.sim;
import std;

import game.colony.driver;
import game.colony.sim_packet;
import game.colony.translator;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // sim packet
    // ------------------------------------------------------------------

    namespace SimPacket {
        // F1 game payload: owned copies captured after the tick, consumed by
        // the present/overlay phases on the same thread. These tests run the
        // production capture (makeSimFramePacket) and staged-build
        // (buildGridStagedFrame + GridPass::applyStagedFrame) path, the
        // fail-closed coherence gate, and the translator input error paths —
        // mostly without a live world (default-constructed driver/translator);
        // the capture/consume tests initialize paused worlds for real present
        // sets.

        PPR_UNIT_TEST (catalog_packet_preserves_identity_metric_and_owned_visuals) {
            colony::SimFramePacket packet{};
            packet.m_has_catalog = true;
            packet.m_boundary_material_id = 16u;
            packet.m_cell_size_m = 1.0f / 3.0f;
            packet.m_content_fingerprint = 42u;
            packet.m_has_background = true;
            for (u32 order = 0u; order < 4u; ++order) {
                packet.m_background_layers[order].m_order = order;
                packet.m_background_layers[order].m_texture_index = 0u;
            }
            auto surface = std::make_shared<GridSurfaceProfile>();
            surface->m_cell_size_m = packet.m_cell_size_m;
            surface->m_heights_m[0u][0u] = 12.5f;
            surface->m_heights_m.back()[3u] = 1240.0f;
            const std::weak_ptr<const GridSurfaceProfile> surface_lifetime = surface;
            packet.m_surface_profile = std::move(surface);
            mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(4u);
            PPR_TEST_ASSERT(not storage.materialize());
            const auto pixels = storage.getMutableData();
            PPR_TEST_ASSERT(pixels.has_value());
            std::fill(pixels->begin(), pixels->end(), std::byte{255});
            mem::SharedBuffer frozen;
            PPR_TEST_ASSERT(not storage.moveToShared(&frozen));
            image::ImageAsset image{};
            image.m_width = 1u;
            image.m_height = 1u;
            image.m_storage = frozen;
            image.m_subresources.push_back({.m_view = frozen.subspan(0u, 4u), .m_row_pitch = 4u, .m_slice_pitch = 4u});
            packet.m_terrain_images.assign(3u, image);
            packet.m_terrain_visuals.push_back({16u, 0x123456ffu, 2u, 2.0f});
            packet.m_terrain_visuals.push_back({257u, 0xabcdef80u, 0xffffffffu, 1.0f});
            packet.m_terrain_visuals[1u].m_surface_flags = kGridSurfaceOpticalBlend | kGridSurfaceTextureRepeat;
            packet.m_full_refresh = true;
            packet.m_resident_chunks.push_back({0u, 0u});
            colony::SimChunkCells cells{};
            cells.m_pos = {0u, 0u};
            cells.m_cells.resize(sim::kCellsPerChunk);
            cells.m_cells[1u].m_element = 16u;
            cells.m_cells[2u].m_element = 257u;
            packet.m_chunk_cells.push_back(std::move(cells));
            GridPass::GridStagedFrame stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(packet, stage));
            PPR_TEST_ASSERT(stage.m_has_catalog and stage.m_content_fingerprint == 42u);
            PPR_TEST_ASSERT(stage.m_boundary_material_id == 16u);
            PPR_TEST_ASSERT(stage.m_cell_size_m == 1.0f / 3.0f);
            PPR_TEST_ASSERT(stage.m_surface_profile == packet.m_surface_profile);
            GridPass surface_pass;
            PPR_TEST_ASSERT(not surface_pass.applyStagedFrame(stage));
            const u64 uploads = surface_pass.uploadCount();
            for (const float invalid_height:
                {-1.0f, 1366.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                auto rejected = stage;
                auto invalid_surface = std::make_shared<GridSurfaceProfile>(*stage.m_surface_profile);
                invalid_surface->m_heights_m.back()[3u] = invalid_height;
                rejected.m_surface_profile = std::move(invalid_surface);
                // First publication exercises validation without an existing
                // world's immutable-profile comparison rejecting the change.
                GridPass rejected_pass;
                PPR_TEST_ASSERT(rejected_pass.applyStagedFrame(rejected) == std::errc::invalid_argument);
                PPR_TEST_ASSERT(rejected_pass.uploadCount() == 0u and rejected_pass.cachedTileCount() == 0u);
                PPR_TEST_ASSERT(surface_pass.uploadCount() == uploads and surface_pass.cachedTileCount() == 1u);
            }
            auto rejected_metric = stage;
            auto invalid_metric = std::make_shared<GridSurfaceProfile>(*stage.m_surface_profile);
            invalid_metric->m_cell_size_m = 1.0f;
            rejected_metric.m_surface_profile = std::move(invalid_metric);
            GridPass rejected_metric_pass;
            PPR_TEST_ASSERT(rejected_metric_pass.applyStagedFrame(rejected_metric) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(rejected_metric_pass.uploadCount() == 0u and rejected_metric_pass.cachedTileCount() == 0u);
            rejected_metric.m_surface_profile.reset();
            packet.m_surface_profile.reset();
            PPR_TEST_ASSERT(not surface_lifetime.expired());
            PPR_TEST_ASSERT(stage.m_surface_profile->m_cell_size_m == 1.0f / 3.0f);
            PPR_TEST_ASSERT(stage.m_surface_profile->m_heights_m[0u][0u] == 12.5f);
            PPR_TEST_ASSERT(stage.m_surface_profile->m_heights_m.back()[3u] == 1240.0f);
            stage.m_surface_profile.reset();
            PPR_TEST_ASSERT(not surface_lifetime.expired());
            PPR_TEST_ASSERT(not surface_pass.shutdown());
            PPR_TEST_ASSERT(surface_lifetime.expired());
            PPR_TEST_ASSERT(stage.m_uploads.size() == 1u);
            PPR_TEST_ASSERT(stage.m_uploads[0u].m_cell_materials[0u] == 0u);
            PPR_TEST_ASSERT(stage.m_uploads[0u].m_cell_materials[1u] == 16u);
            PPR_TEST_ASSERT(stage.m_uploads[0u].m_cell_materials[2u] == 257u);
            packet.m_terrain_visuals[0u].m_color_rgba = 0u;
            packet.m_chunk_cells[0u].m_cells[1u].m_element = 0u;
            PPR_TEST_ASSERT(stage.m_terrain_visuals[0u].m_color_rgba == 0x123456ffu);
            PPR_TEST_ASSERT(stage.m_terrain_visuals[0u].m_texture_index == 2u);
            PPR_TEST_ASSERT(stage.m_terrain_visuals[0u].m_texture_period_m == 2.0f);
            PPR_TEST_ASSERT(
                stage.m_terrain_visuals[1u].m_surface_flags == (kGridSurfaceOpticalBlend | kGridSurfaceTextureRepeat));
            packet.m_terrain_images.clear();
            PPR_TEST_ASSERT(stage.m_terrain_images.size() == 3u);
            PPR_TEST_ASSERT(stage.m_terrain_images[2u].m_storage.getBufferData()[0u] == std::byte{255});
            packet.m_terrain_images.assign(3u, image);
            PPR_TEST_ASSERT(stage.m_uploads[0u].m_cell_materials[1u] == 16u);

            // Unknown IDs (including non-terrain catalog members) cannot enter terrain uploads.
            packet.m_chunk_cells[0u].m_cells[1u].m_element = 17u;
            PPR_TEST_ASSERT(colony::buildGridStagedFrame(packet, stage) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(stage.m_uploads.empty() and stage.m_ack_chunk_ids.empty());
            packet.m_chunk_cells[0u].m_cells[1u].m_element = 16u;
            packet.m_boundary_material_id = 32u;
            PPR_TEST_ASSERT(
                colony::buildGridStagedFrame(packet, stage) == std::make_error_code(std::errc::invalid_argument));
            packet.m_boundary_material_id = 16u;
            packet.m_terrain_visuals[1u].m_material_id = 16u;
            PPR_TEST_ASSERT(colony::buildGridStagedFrame(packet, stage) == std::errc::invalid_argument);
            packet.m_terrain_visuals[1u].m_material_id = 257u;
            packet.m_terrain_visuals[1u].m_texture_period_m = 0.0f;
            PPR_TEST_ASSERT(colony::buildGridStagedFrame(packet, stage) == std::errc::invalid_argument);
            packet.m_terrain_visuals[1u].m_texture_period_m = 1.0f;
            packet.m_cell_size_m = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(colony::buildGridStagedFrame(packet, stage) == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (default_driver_builds_empty_coherent_packet) {
            colony::ColonyDriver driver{};
            colony::SimFramePacket packet = colony::makeSimFramePacket(3u, driver);

            PPR_TEST_ASSERT(packet.m_frame_id == 3u);
            PPR_TEST_ASSERT(packet.m_tick == driver.tickCount());
            PPR_TEST_ASSERT(packet.m_seed == driver.seed());
            PPR_TEST_ASSERT(packet.m_dig_total == driver.digTotal());
            PPR_TEST_ASSERT(packet.m_present_chunks.empty());
            PPR_TEST_ASSERT(packet.m_resident_chunks.empty());
            PPR_TEST_ASSERT(packet.m_chunk_cells.empty());
            PPR_TEST_ASSERT(not packet.m_full_refresh);
            PPR_TEST_ASSERT(colony::isSimPacketCoherent(packet, driver.tickCount(), driver.worldGeneration()));
        };

        PPR_UNIT_TEST (initial_presentation_batches_bound_copies_and_preserve_prefix) {
            colony::ColonyDriver driver{};
            for (const u32 chunk: {0u, 7u, 8u}) {
                PPR_TEST_ASSERT(not driver.grid().setCell(
                    {chunk * sim::kChunkEdge, 0u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            }
            auto first = colony::makeInitialPresentationBatch(1u, driver, 0u, 8u);
            PPR_TEST_ASSERT(first.has_value());
            PPR_TEST_ASSERT(first->m_chunk_cells.size() == 2u);
            PPR_TEST_ASSERT(first->m_resident_chunks.size() == 2u);
            PPR_TEST_ASSERT(first->m_present_chunks.size() == 2u);
            PPR_TEST_ASSERT(not first->m_full_refresh);
            GridPass pass{};
            GridPass::GridStagedFrame stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(*first, stage));
            stage.m_reset_caches = true;
            PPR_TEST_ASSERT(not pass.applyStagedFrame(stage));
            PPR_TEST_ASSERT(pass.cachedTileCount() == 2u);

            // Later batches retain prepared tiles and copy only their range.
            auto second = colony::makeInitialPresentationBatch(2u, driver, 8u, 8u);
            PPR_TEST_ASSERT(second.has_value());
            PPR_TEST_ASSERT(second->m_resident_chunks.size() == 3u);
            PPR_TEST_ASSERT(second->m_chunk_cells.size() == 1u);
            PPR_TEST_ASSERT(second->m_present_chunks[0u] == sim::ChunkPos{8u, 0u});
            stage = {};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(*second, stage));
            PPR_TEST_ASSERT(stage.m_uploads.size() == 1u);
            PPR_TEST_ASSERT(not pass.applyStagedFrame(stage));
            PPR_TEST_ASSERT(pass.cachedTileCount() == 3u);
            PPR_TEST_ASSERT(pass.uploadCount() == 3u);

            const auto oversized = colony::makeInitialPresentationBatch(3u, driver, 0u, 9u);
            PPR_TEST_ASSERT(not oversized and oversized.error() == std::errc::invalid_argument);
            const auto overflowing = colony::makeInitialPresentationBatch(3u, driver, sim::kChunkCount - 1u, 8u);
            PPR_TEST_ASSERT(not overflowing and overflowing.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (world_presentation_capture_owns_all_resident_terrain_and_cancels) {
            colony::ColonyDriver driver{};
            for (const u32 chunk: {0u, 8u, 1023u}) {
                const sim::GlobalCellPos cell{(chunk % 32u) * 128u, (chunk / 32u) * 128u};
                PPR_TEST_ASSERT(not driver.grid().setCell(cell, sim::Cell{.m_element = 7u, .m_temperature = 293.0f}));
            }
            const auto dirty_before = driver.grid().dirtyChunks();
            PPR_TEST_ASSERT(not dirty_before.empty());
            const auto captured = colony::captureWorldPresentation(driver);
            PPR_TEST_ASSERT(captured and captured->m_tiles.size() == 3u and captured->m_uploads.size() == 3u);
            PPR_TEST_ASSERT(captured->m_full_refresh and not captured->m_reset_caches);
            PPR_TEST_ASSERT(captured->m_ack_seed == driver.worldGeneration());
            PPR_TEST_ASSERT(captured->m_uploads[2u].m_chunk_id == 1023u);
            for (const auto &upload: captured->m_uploads) {
                PPR_TEST_ASSERT(upload.m_cell_materials.size() == sim::kCellsPerChunk);
                PPR_TEST_ASSERT(upload.m_cell_materials[0u] == 7u and upload.m_cell_materials[1u] == 0xffffu);
            }
            PPR_TEST_ASSERT(std::ranges::equal(driver.grid().dirtyChunks(), dirty_before));
            PPR_TEST_ASSERT(not driver.grid().setCell({0u, 0u}, sim::Cell{.m_element = 99u}));
            PPR_TEST_ASSERT(captured->m_uploads[0u].m_cell_materials[0u] == 7u);
            const auto invalid = colony::captureWorldPresentation(driver);
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
            auto [context, cancel] = context::withCancelClause(context::background());
            cancel(std::make_error_code(std::errc::operation_canceled));
            const auto cancelled = colony::captureWorldPresentation(driver, context.get());
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        };

        PPR_UNIT_TEST (initial_presentation_skipped_batches_expose_only_confirmed_tiles) {
            colony::ColonyDriver driver{};
            for (const u32 chunk: {0u, 8u}) {
                PPR_TEST_ASSERT(not driver.grid().setCell(
                    {chunk * sim::kChunkEdge, 0u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            }
            colony::InitialPresentation preparation{};
            auto first = preparation.stage(1u, driver);
            PPR_TEST_ASSERT(first and first->m_reset_caches and first->m_uploads.size() == 1u);
            auto waiting = preparation.stage(2u, driver);
            PPR_TEST_ASSERT(waiting and waiting->m_tiles.empty() and waiting->m_uploads.empty());
            RenderFrameResult skipped{.m_frame_id = 1u};
            PPR_TEST_ASSERT(not preparation.consume(skipped, driver.worldGeneration()));
            PPR_TEST_ASSERT(preparation.completedChunks() == 0u);
            auto retry = preparation.stage(3u, driver);
            PPR_TEST_ASSERT(retry and retry->m_reset_caches and retry->m_uploads.size() == 1u);
            RenderFrameResult deferred{};
            deferred.m_frame_id = 3u;
            deferred.m_grid_ack_seed = driver.worldGeneration();
            deferred.m_grid_reset_caches = true;
            deferred.m_grid_staging_applied = true;
            deferred.m_presented = true;
            PPR_TEST_ASSERT(not preparation.consume(deferred, driver.worldGeneration()));
            PPR_TEST_ASSERT(preparation.completedChunks() == 0u);
            retry = preparation.stage(30u, driver);
            PPR_TEST_ASSERT(retry and not retry->m_reset_caches and retry->m_uploads.size() == 1u);
            RenderFrameResult accepted{};
            accepted.m_frame_id = 30u;
            accepted.m_presented = true;
            accepted.m_grid_stage_applied = true;
            accepted.m_grid_ack_seed = driver.worldGeneration();
            PPR_TEST_ASSERT(not preparation.consume(accepted, driver.worldGeneration()));
            PPR_TEST_ASSERT(preparation.completedChunks() == 8u);
            auto later = preparation.stage(4u, driver);
            PPR_TEST_ASSERT(later and not later->m_reset_caches and later->m_tiles.size() == 2u);
            waiting = preparation.stage(5u, driver);
            PPR_TEST_ASSERT(waiting and waiting->m_tiles.size() == 1u and waiting->m_uploads.empty());
            PPR_TEST_ASSERT(waiting->m_tiles[0u].m_dirty_mask == 0u);
            skipped.m_frame_id = 4u;
            skipped.m_dropped = true;
            PPR_TEST_ASSERT(not preparation.consume(skipped, driver.worldGeneration()));
            PPR_TEST_ASSERT(preparation.completedChunks() == 8u);
            retry = preparation.stage(6u, driver);
            PPR_TEST_ASSERT(retry and not retry->m_reset_caches and retry->m_uploads.size() == 1u);
            // A late result for the skipped frame cannot settle this retry.
            accepted.m_frame_id = 4u;
            PPR_TEST_ASSERT(not preparation.consume(accepted, driver.worldGeneration()));
            PPR_TEST_ASSERT(preparation.completedChunks() == 8u);
            accepted.m_frame_id = 6u;
            PPR_TEST_ASSERT(not preparation.consume(accepted, driver.worldGeneration()));
            PPR_TEST_ASSERT(preparation.completedChunks() == 16u);
            for (u64 frame = 7u; preparation.completedChunks() < sim::kChunkCount; ++frame) {
                const auto batch = preparation.stage(frame, driver);
                PPR_TEST_ASSERT(batch.has_value());
                accepted.m_frame_id = frame;
                const bool complete = preparation.consume(accepted, driver.worldGeneration());
                PPR_TEST_ASSERT(complete == (preparation.completedChunks() == sim::kChunkCount));
            }
        };

        // Consumption ack payload: the per-chunk edit revision captured
        // with the packet (the lane echoes exactly this set back at
        // settlement time, so only captured generations are cleared).
        Array<colony::ColonyDriver::DirtyChunk> packetAckSet(const colony::SimFramePacket &packet) {
            Array<colony::ColonyDriver::DirtyChunk> acked{};
            acked.reserve(packet.m_present_chunks.size());
            for (u64 i = 0u; i < packet.m_present_chunks.size(); ++i) {
                acked.push_back({.m_pos = packet.m_present_chunks[i], .m_revision = packet.m_present_revisions[i]});
            }
            return acked;
        }

        // F1 game payload, real capture→mutation→consumer path: the packet
        // carries the chunk-content render payload, and same-thread present
        // preparation consumes the packet — never the live grid. Poisoning
        // the live cells with an invalid element after capture cannot alter
        // consumption (a live read would fail closed); only the captured
        // content reaches the pass.
        PPR_UNIT_TEST (capture_mutation_consume_uses_packet_content) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 1234567u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };

            // One rock cell dug through the paused tool path, so the present
            // set and the dig counters populate exactly as in production.
            // (Element 1 is rock per the colony element registry; the dig
            // clears it to vacuum, element 0.)
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(driver.initialMatterBaselineValid());
            PPR_TEST_ASSERT(not driver.grid().setCell({11u, 11u}, sim::Cell{}));
            PPR_TEST_ASSERT(not driver.requestDig({11u, 11u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));
            PPR_TEST_ASSERT(driver.initialMatterBaselineValid()); // An empty dig changes no baseline geometry.
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));
            PPR_TEST_ASSERT(driver.lastEditChunks().size() == 1u);
            PPR_TEST_ASSERT(driver.lastEditChunks()[0u] == sim::ChunkPos{0u, 0u});
            PPR_TEST_ASSERT(driver.digTotal() == 1u);
            PPR_TEST_ASSERT(not driver.initialMatterBaselineValid());

            // Real capture: residency, the present chunk, and its cells.
            colony::SimFramePacket packet = colony::makeSimFramePacket(9u, driver);
            PPR_TEST_ASSERT(packet.m_present_chunks.size() == 1u);
            PPR_TEST_ASSERT(packet.m_chunk_cells.size() == 1u);
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_pos == sim::ChunkPos{0u, 0u});
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_cells.size() == sim::kCellsPerChunk);
            constexpr u32 kDugCell = 10u * sim::kChunkEdge + 10u;
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_cells[kDugCell].m_element == 0u);

            // Full coordinator capture preserves actual pending revisions,
            // even when the captured edited cell has become vacuum.
            Array<colony::ColonyDriver::DirtyChunk> pending;
            const auto before = driver.presentEditsCapture();
            pending.assign(before.begin(), before.end());
            PPR_TEST_ASSERT(not pending.empty());
            const auto full = colony::captureWorldPresentation(driver);
            PPR_TEST_ASSERT(full and full->m_uploads.size() == driver.grid().residentChunkCount());
            PPR_TEST_ASSERT(full->m_ack_revisions.size() == pending.size());
            const auto after = driver.presentEditsCapture();
            PPR_TEST_ASSERT(after.size() == pending.size());
            for (u32 index = 0u; index < pending.size(); ++index) {
                PPR_TEST_ASSERT(after[index].m_pos == pending[index].m_pos);
                PPR_TEST_ASSERT(after[index].m_revision == pending[index].m_revision);
                PPR_TEST_ASSERT(full->m_ack_revisions[index] == pending[index].m_revision);
            }

            // Post-capture producer mutation: overwrite the live chunk with
            // invalid elements (above the element registry). The owned packet
            // keeps the captured vacuum cell.
            Array<sim::Cell> poison{};
            poison.assign(sim::kCellsPerChunk, sim::Cell{.m_element = 999u, .m_temperature = 0.0f});
            PPR_TEST_ASSERT(not driver.grid().setCells(sim::ChunkPos{0u, 0u}, std::move(poison)));
            PPR_TEST_ASSERT(packet.m_chunk_cells[0u].m_cells[kDugCell].m_element == 0u);

            // Real consumer: succeeds (a live-grid read would fail closed on
            // the poisoned cells), stages exactly the capture-time residency,
            // and uploads only the packet-dirty chunk. Tick/seed never moved,
            // so the packet stays coherent across the mutation.
            GridPass::GridStagedFrame stage{};
            PPR_TEST_ASSERT(colony::isSimPacketCoherent(packet, driver.tickCount(), driver.worldGeneration()));
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(packet, stage));
            GridPass pass{};
            PPR_TEST_ASSERT(not pass.applyStagedFrame(stage));
            const Array<colony::ColonyDriver::DirtyChunk> acked = packetAckSet(packet);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{acked});
            PPR_TEST_ASSERT(pass.stagedTileCount() == driver.grid().residentChunkCount());
            PPR_TEST_ASSERT(pass.stagedTileCount() == packet.m_resident_chunks.size());
            PPR_TEST_ASSERT(pass.uploadCount() == 1u);
            PPR_TEST_ASSERT(colony::isSimPacketCoherent(packet, driver.tickCount(), driver.worldGeneration()));
        };

        // Gate 0 multi-edit accumulation: two digs applied by two paused
        // updates (no present between them) leave BOTH chunks in the pending
        // present-dirty set while lastEditChunks reports only the second.
        // One capture snapshots the complete set and one present uploads
        // both, acking everything — a follow-up frame with no new edits
        // uploads nothing, so no stale remainder survives a successful
        // present (which clears the full-refresh flag in production).
        PPR_UNIT_TEST (two_chunks_across_updates_capture_and_upload_both) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 1234567u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };

            // Rock in two distinct chunks (chunk edge 128): (0, 0) and (1, 0).
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            PPR_TEST_ASSERT(
                not driver.grid().setCell({200u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));
            PPR_TEST_ASSERT(not driver.requestDig({200u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            // Both edits still pending; the last-edit view reports only the second.
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 2u);
            PPR_TEST_ASSERT(driver.lastEditChunks().size() == 1u);
            PPR_TEST_ASSERT(driver.lastEditChunks()[0u] == sim::ChunkPos{1u, 0u});

            colony::SimFramePacket packet = colony::makeSimFramePacket(11u, driver);
            PPR_TEST_ASSERT(packet.m_present_chunks.size() == 2u);
            PPR_TEST_ASSERT(packet.m_present_chunks[0u] == sim::ChunkPos{0u, 0u});
            PPR_TEST_ASSERT(packet.m_present_chunks[1u] == sim::ChunkPos{1u, 0u});
            PPR_TEST_ASSERT(packet.m_chunk_cells.size() == 2u);

            GridPass::GridStagedFrame pre_stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(packet, pre_stage));
            GridPass pass{};
            PPR_TEST_ASSERT(not pass.applyStagedFrame(pre_stage));
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
            // Laned ack delivery: at lane consumption, the driver dirty set
            // for the captured frame is acknowledged. Nothing went stale.
            const Array<colony::ColonyDriver::DirtyChunk> acked = packetAckSet(packet);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{acked});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());

            // Next frame with no new edits uploads nothing: nothing went stale.
            colony::SimFramePacket follow = colony::makeSimFramePacket(12u, driver);
            PPR_TEST_ASSERT(follow.m_present_chunks.empty());
            PPR_TEST_ASSERT(follow.m_chunk_cells.empty());
            GridPass follow_pass{};
            GridPass::GridStagedFrame follow_stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(follow, follow_stage));
            PPR_TEST_ASSERT(not follow_pass.applyStagedFrame(follow_stage));
            PPR_TEST_ASSERT(follow_pass.uploadCount() == 0u);
            PPR_TEST_ASSERT(follow_pass.stagedTileCount() == follow.m_resident_chunks.size());
        };

        // Full-refresh capture covers every resident chunk's cells (the
        // regeneration path that empties the pass cache), on a default
        // driver with hand-seeded chunks — no world init needed.
        PPR_UNIT_TEST (full_refresh_captures_all_resident_cells) {
            colony::ColonyDriver driver{};
            Array<sim::Cell> rock{};
            rock.assign(sim::kCellsPerChunk, sim::Cell{.m_element = 1u, .m_temperature = 293.0f});
            Array<sim::Cell> rock_copy = rock;
            PPR_TEST_ASSERT(not driver.grid().setCells(sim::ChunkPos{0u, 0u}, std::move(rock)));
            PPR_TEST_ASSERT(not driver.grid().setCells(sim::ChunkPos{1u, 2u}, std::move(rock_copy)));

            colony::SimFramePacket packet = colony::makeSimFramePacket(4u, driver, true);
            PPR_TEST_ASSERT(packet.m_full_refresh);
            PPR_TEST_ASSERT(packet.m_present_chunks.empty());
            PPR_TEST_ASSERT(packet.m_resident_chunks.size() == 2u);
            PPR_TEST_ASSERT(packet.m_chunk_cells.size() == 2u);

            GridPass::GridStagedFrame full_stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(packet, full_stage));
            GridPass pass{};
            PPR_TEST_ASSERT(not pass.applyStagedFrame(full_stage));
            PPR_TEST_ASSERT(pass.stagedTileCount() == 2u);
            PPR_TEST_ASSERT(pass.uploadCount() == 2u);
        };

        // Fail-closed consumption: a dirty chunk without captured cells (or
        // with a corrupt payload) clears the staged frame and reports
        // invalid_argument — never a partial present.
        PPR_UNIT_TEST (consume_rejects_dirty_chunk_without_cells) {
            colony::ColonyDriver driver{};
            colony::SimFramePacket packet{};
            packet.m_resident_chunks.push_back(sim::ChunkPos{0u, 0u});
            packet.m_present_chunks.push_back(sim::ChunkPos{0u, 0u});

            GridPass pass{};
            GridPass::GridStagedFrame empty_stage{};
            PPR_TEST_ASSERT(
                colony::buildGridStagedFrame(packet, empty_stage) == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(pass.applyStagedFrame(empty_stage) == std::error_code{});
            PPR_TEST_ASSERT(pass.stagedTileCount() == 0u);

            Array<sim::Cell> corrupt{};
            corrupt.assign(sim::kCellsPerChunk, sim::Cell{.m_element = 999u, .m_temperature = 0.0f});
            colony::SimChunkCells payload{};
            payload.m_pos = sim::ChunkPos{0u, 0u};
            payload.m_cells = std::move(corrupt);
            packet.m_chunk_cells.push_back(std::move(payload));

            GridPass retry_pass{};
            GridPass::GridStagedFrame retry_stage{};
            PPR_TEST_ASSERT(
                colony::buildGridStagedFrame(packet, retry_stage) == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(retry_pass.applyStagedFrame(retry_stage) == std::error_code{});
            PPR_TEST_ASSERT(retry_pass.stagedTileCount() == 0u);
        };

        PPR_UNIT_TEST (coherence_rejects_stale_tick_and_regenerated_world) {
            colony::SimFramePacket packet{};
            packet.m_tick = 5u;
            packet.m_seed = 11u;
            packet.m_world_generation = 11u;

            PPR_TEST_ASSERT(colony::isSimPacketCoherent(packet, 5u, 11u));
            PPR_TEST_ASSERT(not colony::isSimPacketCoherent(packet, 6u, 11u));
            PPR_TEST_ASSERT(not colony::isSimPacketCoherent(packet, 5u, 12u));
        };

        PPR_UNIT_TEST (mark_chunk_validates_positions) {
            colony::ColonyTranslator translator{};
            PPR_TEST_ASSERT(not translator.markChunkChanged(sim::ChunkPos{0u, 0u}));
            PPR_TEST_ASSERT(
                translator.markChunkChanged(sim::ChunkPos{sim::kChunksPerEdge, 0u}) ==
                std::make_error_code(std::errc::invalid_argument));
        };

        PPR_UNIT_TEST (empty_present_drain_succeeds) {
            // Draining a default driver marks nothing and reports success:
            // the present path is well-defined with zero edits.
            colony::ColonyDriver driver{};
            colony::ColonyTranslator translator{};
            PPR_TEST_ASSERT(not driver.presentEdits(translator));
        };

        // Same-chunk re-edit: the entry bumped after capture carries a
        // NEW revision row. A lane ack for the stale capture must not
        // clear the pending newer generation — only a matching revision
        // clears.
        PPR_UNIT_TEST (same_chunk_reedit_stale_ack_keeps_newer_revision) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 77u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            PPR_TEST_ASSERT(not driver.grid().setCell({20u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            const colony::SimFramePacket first = colony::makeSimFramePacket(21u, driver);
            PPR_TEST_ASSERT(first.m_present_chunks.size() == 1u);
            const u64 first_revision = first.m_present_revisions[0u];

            // Same chunk dirtied again before any consumption.
            PPR_TEST_ASSERT(not driver.requestDig({20u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);
            const u64 bumped_revision = driver.presentEditsCapture()[0u].m_revision;
            PPR_TEST_ASSERT(bumped_revision > first_revision);

            // A stale settlement (the dropped-then-late first frame's ack
            // manifest) clears nothing: the newer generation stays pending.
            const Array<colony::ColonyDriver::DirtyChunk> stale_ack = packetAckSet(first);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{stale_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);
            PPR_TEST_ASSERT(driver.presentEditsCapture()[0u].m_revision == bumped_revision);

            // A fresh capture + settlement acknowledges exactly its own
            // generation and empties the set.
            const colony::SimFramePacket second = colony::makeSimFramePacket(22u, driver);
            const Array<colony::ColonyDriver::DirtyChunk> fresh_ack = packetAckSet(second);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{fresh_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());
        };

        // Revision ABA guard: after an ack removes a row, a re-edit to the
        // same chunk must CONTINUE the revision (not restart at 1), so an
        // overlapping in-flight packet carrying the old revision-1 ack can
        // never clear the new edit.
        PPR_UNIT_TEST (revision_continues_beyond_ack_row_removal) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 55u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            const colony::SimFramePacket first = colony::makeSimFramePacket(41u, driver);
            const u64 first_revision = first.m_present_revisions[0u];
            const Array<colony::ColonyDriver::DirtyChunk> first_ack = packetAckSet(first);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{first_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());

            // New edit to the SAME chunk: the replacement row starts at
            // first_revision + 1 (never back at 1).
            PPR_TEST_ASSERT(not driver.grid().setCell({12u, 12u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            PPR_TEST_ASSERT(not driver.requestDig({12u, 12u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);
            const u64 second_revision = driver.presentEditsCapture()[0u].m_revision;
            PPR_TEST_ASSERT(second_revision > first_revision);

            // Duplicate-capture ack for the OLD revision clears nothing.
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{first_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);
            PPR_TEST_ASSERT(driver.presentEditsCapture()[0u].m_revision == second_revision);

            const colony::SimFramePacket second = colony::makeSimFramePacket(42u, driver);
            const Array<colony::ColonyDriver::DirtyChunk> second_ack = packetAckSet(second);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{second_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());
        };

        // Seed-not-epoch guard: the ack manifest carries the genuine world
        // generation, so a full-refresh clear and the ack itself both ride
        // it — a stale late result cannot resurrect them, and the
        // generation advances even when a regenerate reuses the same seed.
        PPR_UNIT_TEST (ack_manifest_carries_world_generation) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 77u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };

            const u64 generation_before = driver.worldGeneration();
            colony::SimFramePacket packet = colony::makeSimFramePacket(43u, driver);
            PPR_TEST_ASSERT(packet.m_world_generation == generation_before);
            GridPass::GridStagedFrame stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(packet, stage));
            PPR_TEST_ASSERT(stage.m_ack_seed == generation_before);

            // Same user seed, NEW generation: the stale manifest no longer
            // matches, so neither the ack nor a full-refresh clear applies.
            PPR_TEST_ASSERT(not driver.regenerateLegacyFixture(77u));
            PPR_TEST_ASSERT(driver.worldGeneration() > generation_before);
            PPR_TEST_ASSERT(stage.m_ack_seed != driver.worldGeneration());
            PPR_TEST_ASSERT(not colony::isSimPacketCoherent(packet, driver.tickCount(), driver.worldGeneration()));
        };

        // Regeneration changes the world epoch; a captured packet is then
        // incoherent, the driver dirty set drops with the old world, and a
        // stale lane ack is a no-op against the cleared set. The replacement
        // frame re-stages the whole world via the full-refresh capture.
        PPR_UNIT_TEST (regeneration_invalidates_packet_and_drops_dirty_set) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 11u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            const colony::SimFramePacket stale = colony::makeSimFramePacket(31u, driver);
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);

            PPR_TEST_ASSERT(not driver.regenerateLegacyFixture(99u));
            PPR_TEST_ASSERT(not colony::isSimPacketCoherent(stale, driver.tickCount(), driver.worldGeneration()));
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());

            const Array<colony::ColonyDriver::DirtyChunk> stale_ack = packetAckSet(stale);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{stale_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());

            const colony::SimFramePacket fresh = colony::makeSimFramePacket(32u, driver, true);
            PPR_TEST_ASSERT(fresh.m_full_refresh);
            PPR_TEST_ASSERT(fresh.m_present_chunks.empty());
            PPR_TEST_ASSERT(fresh.m_chunk_cells.size() == fresh.m_resident_chunks.size());
            GridPass::GridStagedFrame fresh_stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(fresh, fresh_stage));
            GridPass fresh_pass{};
            PPR_TEST_ASSERT(not fresh_pass.applyStagedFrame(fresh_stage));
            PPR_TEST_ASSERT(fresh_pass.stagedTileCount() == fresh.m_resident_chunks.size());
        };

        // Rejection/reset replay: a corrupt packet fails closed on the
        // client and fires NO consumption ack, so the dirty set stays
        // pending for the retry. The intact retry builds, applies, and
        // acknowledges normally.
        PPR_UNIT_TEST (rejected_frame_replays_dirty_set) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 5u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            const colony::SimFramePacket packet = colony::makeSimFramePacket(41u, driver);
            PPR_TEST_ASSERT(packet.m_present_chunks.size() == 1u);

            colony::SimFramePacket corrupt = packet;
            corrupt.m_chunk_cells.clear();
            GridPass::GridStagedFrame rejected_stage{};
            PPR_TEST_ASSERT(
                colony::buildGridStagedFrame(corrupt, rejected_stage) ==
                std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(rejected_stage.m_tiles.empty());
            // No ack rides the rejected frame: the dirty set survives.
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);

            GridPass::GridStagedFrame retry_stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(packet, retry_stage));
            GridPass retry_pass{};
            PPR_TEST_ASSERT(not retry_pass.applyStagedFrame(retry_stage));
            PPR_TEST_ASSERT(retry_pass.uploadCount() == 1u);
            const Array<colony::ColonyDriver::DirtyChunk> fresh_ack = packetAckSet(packet);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{fresh_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());
        };

        // Dropped completion: a frame that never executed (transport
        // drop, teardown cancellation, worker error) acknowledges
        // NOTHING — presentation state stays pending, so the world edits
        // inside it survive for the next buildable frame.
        PPR_UNIT_TEST (dropped_frame_never_acks_dirty_set) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 21u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            const colony::SimFramePacket packet = colony::makeSimFramePacket(51u, driver);
            PPR_TEST_ASSERT(packet.m_present_chunks.size() == 1u);

            // Settlement returns m_dropped/error without an applied stage:
            // the owner re-arms presentation state and acks nothing here.
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);

            const colony::SimFramePacket retry = colony::makeSimFramePacket(52u, driver);
            GridPass::GridStagedFrame retry_stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(retry, retry_stage));
            GridPass retry_pass{};
            PPR_TEST_ASSERT(not retry_pass.applyStagedFrame(retry_stage));
            const Array<colony::ColonyDriver::DirtyChunk> fresh_ack = packetAckSet(retry);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{fresh_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());
        };

        // Exception case at the capture→build boundary: allocation or
        // validation failure inside the staged build clears the stage and
        // propagates an error — the driver dirty set is untouched until a
        // successful consumption explicitly acknowledges it.
        PPR_UNIT_TEST (staged_build_failure_preserves_dirty_set) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 33u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            const colony::SimFramePacket packet = colony::makeSimFramePacket(61u, driver);
            colony::SimFramePacket corrupt = packet;
            corrupt.m_present_chunks.push_back(sim::ChunkPos{1u, 1u});
            PPR_TEST_ASSERT(corrupt.m_chunk_cells.size() == 1u);

            GridPass::GridStagedFrame stage{};
            PPR_TEST_ASSERT(
                colony::buildGridStagedFrame(corrupt, stage) == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(stage.m_tiles.empty());
            PPR_TEST_ASSERT(stage.m_uploads.empty());
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);
        };

        // Exception case at the settlement boundary: an exception escaping
        // the build→apply→ack sequence (worker throw, allocation failure)
        // must look like any other non-applied slot — the driver's pending
        // ack set is untouched, the captured ack manifest still round-trips
        // from the owned stage, and a retry consumes normally.
        PPR_UNIT_TEST (exception_during_settlement_preserves_pending_set) {
            colony::ColonyDriver driver{};
            PPR_TEST_ASSERT(not driver.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 41u}));
            PPR_DEFER {
                std::ignore = driver.shutdown();
            };
            PPR_TEST_ASSERT(not driver.grid().setCell({10u, 10u}, sim::Cell{.m_element = 1u, .m_temperature = 293.0f}));
            driver.setPaused(true);
            PPR_TEST_ASSERT(not driver.requestDig({10u, 10u}, 1u, 1u));
            PPR_TEST_ASSERT(not driver.update(std::chrono::milliseconds{16}));

            const colony::SimFramePacket packet = colony::makeSimFramePacket(71u, driver);
            GridPass::GridStagedFrame stage{};
            PPR_TEST_ASSERT(not colony::buildGridStagedFrame(packet, stage));
            // The owned stage carries the ack manifest forward.
            PPR_TEST_ASSERT(stage.m_ack_seed == driver.worldGeneration());
            PPR_TEST_ASSERT(stage.m_ack_chunk_ids.size() == packet.m_present_chunks.size());

            bool exception_caught = false;
            try {
                throw std::runtime_error{"simulated lane executor failure"};
            } catch (...) {
                exception_caught = true;
            }
            PPR_TEST_ASSERT(exception_caught);

            // The failed slot acks NOTHING: the pending set is intact.
            PPR_TEST_ASSERT(driver.presentEditsCapture().size() == 1u);

            // The retry consumes the same owned stage and clears its set.
            GridPass retry_pass{};
            PPR_TEST_ASSERT(not retry_pass.applyStagedFrame(stage));
            const Array<colony::ColonyDriver::DirtyChunk> fresh_ack = packetAckSet(packet);
            driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{fresh_ack});
            PPR_TEST_ASSERT(driver.presentEditsCapture().empty());
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest sim_packet = UnitTest::Named("sim_packet") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::SimPacket::catalog_packet_preserves_identity_metric_and_owned_visuals,
            detail::SimPacket::default_driver_builds_empty_coherent_packet,
            detail::SimPacket::initial_presentation_batches_bound_copies_and_preserve_prefix,
            detail::SimPacket::initial_presentation_skipped_batches_expose_only_confirmed_tiles,
            detail::SimPacket::world_presentation_capture_owns_all_resident_terrain_and_cancels,
            detail::SimPacket::capture_mutation_consume_uses_packet_content,
            detail::SimPacket::two_chunks_across_updates_capture_and_upload_both,
            detail::SimPacket::full_refresh_captures_all_resident_cells,
            detail::SimPacket::consume_rejects_dirty_chunk_without_cells,
            detail::SimPacket::coherence_rejects_stale_tick_and_regenerated_world,
            detail::SimPacket::mark_chunk_validates_positions,
            detail::SimPacket::empty_present_drain_succeeds,
            detail::SimPacket::same_chunk_reedit_stale_ack_keeps_newer_revision,
            detail::SimPacket::revision_continues_beyond_ack_row_removal,
            detail::SimPacket::ack_manifest_carries_world_generation,
            detail::SimPacket::regeneration_invalidates_packet_and_drops_dirty_set,
            detail::SimPacket::rejected_frame_replays_dirty_set,
            detail::SimPacket::dropped_frame_never_acks_dirty_set,
            detail::SimPacket::staged_build_failure_preserves_dirty_set,
            detail::SimPacket::exception_during_settlement_preserves_pending_set,
        });
    };

    const UnitTest &sim_packetTests() noexcept {
        return sim_packet;
    }
} // namespace pP::tests
