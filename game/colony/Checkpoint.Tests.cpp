module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module game.tests;

import engine.asset;
import engine.core;
import engine.sim;
import engine.app;
import engine.image;
import engine.matter;
import engine.shader;
import engine.rhi;
import game.colony.matter;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.checkpoint;
import game.colony.content;
import game.colony.driver;
import game.colony.digtool;
import game.colony.worldgen;
import game.colony.sim_packet;
import game.application;
import game.colony.panel;
import game.ui.model;
import std;

#include "Checkpoint.Tests.Support.h"

namespace pP::game::tests::detail::checkpoint {
    PPR_DEFINE_LOG_CATEGORY(CheckpointTests, debug, none)

    // ------------------------------------------------------------------
    // checkpoint restore, frozen capture and edit publication
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (dig_candidate_preserves_carried_columns_and_emits_original_seam_inventory_once) {
        Fixture fixture;
        const auto *const oxygen = fixture.m_catalog->findMaterial("oxygen");
        PPR_TEST_ASSERT(oxygen != nullptr);

        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        auto &source = checkpoint.m_current_matter;
        source.m_identity.m_publication = 7u;
        source.m_particles[0u].m_displacement_x = 0.125f;
        source.m_particles[0u].m_deformation_gradient = {1.0f, 0.25f, -0.125f, 0.75f};
        source.m_particles[0u].m_deformation_displacement = {0.125f, 0.25f, -0.5f, -0.125f};
        source.m_particles[0u].m_liquid_density = 0.5f;
        source.m_particles[0u].m_log_jp = 0.75f;
        source.m_gas.push_back(
            {5u, 100.5 * source.m_cell_metres, 100.5 * source.m_cell_metres, 0.1, 0.1, 300.0, 1.0, oxygen->m_numeric_id,
                sim::EGenerationMatter::gas});
        const auto original = source;

        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not sim::apply(grid, checkpoint.m_snapshot));
        Array<sim::ChunkGrid::CapturedChunk> edits;
        for (u32 x: {127u, 128u}) {
            auto captured = grid.captureChunk(sim::chunkOf({x, 40u}));
            PPR_TEST_ASSERT(captured.has_value());
            PPR_TEST_ASSERT(not grid.setCell({x, 40u}, {0u, 99.0f}));
            grid.exchangeChunk(*captured);
            edits.push_back(std::move(*captured));
        }

        const auto prepared = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(prepared and prepared->has_value());
        const auto *const candidate = &**prepared;
        PPR_TEST_ASSERT(candidate->m_next_id == 14u and candidate->m_records.size() == 3u);
        PPR_TEST_ASSERT(candidate->m_identity.m_publication == 8u and candidate->m_identity.m_terrain_revision == 2u);
        PPR_TEST_ASSERT(candidate->m_gas == original.m_gas);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 22u>>(candidate->m_particles[0u]) ==
            std::bit_cast<std::array<u32, 22u>>(source.m_particles[0u]));

        for (std::size_t i = 0u; i < 2u; ++i) {
            const u32 x = 127u + static_cast<u32>(i);
            const auto before = *grid.getCell({x, 40u});
            const auto &record = candidate->m_records[i + 1u];
            const double density = fixture.m_catalog->findMaterial(before.m_element)->m_reference_density_kg_m3;
            const double volume = source.m_cell_metres * source.m_cell_metres * source.m_thickness_metres;

            PPR_TEST_ASSERT(record.m_id == 12u + i and record.m_material == before.m_element);
            PPR_TEST_ASSERT(record.m_mass_kg == density * volume and record.m_volume_m3 == volume);
            PPR_TEST_ASSERT(record.m_temperature_k == static_cast<double>(before.m_temperature));
            PPR_TEST_ASSERT(record.m_x_m == (x + 0.5) * source.m_cell_metres);
            PPR_TEST_ASSERT(
                candidate->m_particles[i + 1u].m_material == static_cast<float>(matter::ESourceMaterial::sand));
            PPR_TEST_ASSERT(candidate->m_particles[i + 1u].m_displacement_x == 0.0f);
        }

        const auto retry = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(retry and retry->has_value());
        PPR_TEST_ASSERT((**retry).m_next_id == candidate->m_next_id);
        PPR_TEST_ASSERT((**retry).m_records == candidate->m_records);
        PPR_TEST_ASSERT(source.m_next_id == original.m_next_id and source.m_identity == original.m_identity);
        PPR_TEST_ASSERT(source.m_records == original.m_records);
        PPR_TEST_ASSERT(grid.getCell({127u, 40u})->m_element != 0u and grid.getCell({128u, 40u})->m_element != 0u);
    };

    PPR_UNIT_TEST (new_contour_coverage_rejects_moving_and_gas_but_preserves_prior_overlap) {
        Fixture fixture;
        const auto *const oxygen = fixture.m_catalog->findMaterial("oxygen");
        PPR_TEST_ASSERT(oxygen != nullptr);

        const auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        const auto solid = fixture.m_catalog->findMaterial("basalt")->m_numeric_id;

        for (u32 gas = 0u; gas < 2u; ++gas) {
            auto source = checkpoint.m_current_matter;
            auto record = source.m_records[0u];
            record.m_x_m = 128.0 * source.m_cell_metres;
            record.m_y_m = 40.5 * source.m_cell_metres;
            PPR_TEST_ASSERT(record.m_x_m / source.m_cell_metres == 128.0);
            PPR_TEST_ASSERT(record.m_y_m / source.m_cell_metres == 40.5);
            source.m_records[0u] = record;
            source.m_particles[0u].m_position_x = 131.0f;
            source.m_particles[0u].m_position_y = 43.5f;

            if (gas != 0u) {
                record.m_material = oxygen->m_numeric_id;
                record.m_phase = sim::EGenerationMatter::gas;
                source.m_gas = {record};
                source.m_records.clear();
                source.m_particles.clear();
                source.m_profile.m_particle_count = 0u;
            }

            sim::ChunkGrid grid;
            auto chunk = grid.captureChunk({1u, 0u});
            PPR_TEST_ASSERT(chunk.has_value());
            PPR_TEST_ASSERT(not grid.setCell({128u, 40u}, {solid, 300.0f}));
            grid.exchangeChunk(*chunk);
            Array<sim::ChunkGrid::CapturedChunk> edits;
            edits.push_back(std::move(*chunk));

            const auto rejected = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
            PPR_TEST_ASSERT(rejected and not rejected->has_value());
            PPR_TEST_ASSERT(source.m_next_id == 12u and source.m_identity.m_publication == 0u);
            PPR_TEST_ASSERT(grid.getCell({128u, 40u})->m_element == 0u);
        }

        auto source = checkpoint.m_current_matter;
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({130u, 40u}, {solid, 300.0f}));
        auto chunk = grid.captureChunk({0u, 0u});
        PPR_TEST_ASSERT(chunk.has_value());
        PPR_TEST_ASSERT(not grid.setCell({5u, 5u}, {solid, 300.0f}));
        grid.exchangeChunk(*chunk);
        Array<sim::ChunkGrid::CapturedChunk> edits;
        edits.push_back(std::move(*chunk));

        const auto unrelated = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(unrelated and unrelated->has_value());
        PPR_TEST_ASSERT((**unrelated).m_records == source.m_records);
        PPR_TEST_ASSERT((**unrelated).m_particles.size() == source.m_particles.size());

        // A nonzero catalog particle material is not solid contour occupancy.
        sim::ChunkGrid nonsolid;
        auto non_solid_chunk = nonsolid.captureChunk({1u, 0u});
        PPR_TEST_ASSERT(non_solid_chunk.has_value());
        PPR_TEST_ASSERT(not nonsolid.setCell({130u, 40u}, {oxygen->m_numeric_id, 300.0f}));
        nonsolid.exchangeChunk(*non_solid_chunk);
        Array<sim::ChunkGrid::CapturedChunk> non_solid_edits;
        non_solid_edits.push_back(std::move(*non_solid_chunk));

        const auto ignored = prepareMatterTerrainEdit(source, nonsolid, non_solid_edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(ignored and ignored->has_value());
        PPR_TEST_ASSERT((**ignored).m_records == source.m_records);

        // The rejected cohort includes a locally reserved dug-cell emission;
        // neither its reserved ID nor any carried column escapes rejection.
        sim::ChunkGrid cohort;
        PPR_TEST_ASSERT(not sim::apply(cohort, checkpoint.m_snapshot));
        auto removed = cohort.captureChunk({0u, 0u});
        auto added = cohort.captureChunk({1u, 0u});
        PPR_TEST_ASSERT(removed and added);
        PPR_TEST_ASSERT(not cohort.setCell({127u, 40u}, {0u, 0.0f}));
        PPR_TEST_ASSERT(not cohort.setCell({130u, 40u}, {solid, 300.0f}));
        cohort.exchangeChunk(*removed);
        cohort.exchangeChunk(*added);
        Array<sim::ChunkGrid::CapturedChunk> cohort_edits;
        cohort_edits.push_back(std::move(*removed));
        cohort_edits.push_back(std::move(*added));

        const auto deferred = prepareMatterTerrainEdit(source, cohort, cohort_edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(deferred and not deferred->has_value());
        PPR_TEST_ASSERT(source.m_next_id == checkpoint.m_current_matter.m_next_id);
        PPR_TEST_ASSERT(source.m_identity == checkpoint.m_current_matter.m_identity);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 22u>>(source.m_particles[0u]) ==
            std::bit_cast<std::array<u32, 22u>>(checkpoint.m_current_matter.m_particles[0u]));
        PPR_TEST_ASSERT(cohort.getCell({127u, 40u})->m_element != 0u and cohort.getCell({130u, 40u})->m_element == 0u);
    };

    PPR_UNIT_TEST (v5_round_trip_preserves_all_current_words_and_rebinds_only_world) {
        Fixture fixture;
        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        auto &state = checkpoint.m_current_matter;
        state.m_identity = {91u, 7u, 4u};
        auto &particle = state.m_particles[0u];
        particle.m_displacement_x = 0.125f;
        particle.m_displacement_y = -0.25f;
        particle.m_deformation_gradient = {1.0f, 0.25f, -0.125f, 0.75f};
        particle.m_deformation_displacement = {0.125f, 0.25f, -0.5f, -0.125f};
        particle.m_liquid_density = 0.5f;
        particle.m_lambda = 0.375f;
        particle.m_log_jp = 0.75f;
        const auto words = std::bit_cast<std::array<u32, 22u>>(particle);

        const auto bytes = encodeWorldCheckpoint(checkpoint, *fixture.m_catalog);
        PPR_TEST_ASSERT(bytes.has_value());
        const auto decoded = decodeWorldCheckpoint(*bytes, *fixture.m_catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 22u>>(decoded->m_current_matter.m_particles[0u]) == words);
        PPR_TEST_ASSERT(decoded->m_current_matter.m_identity == state.m_identity);

        const auto snapshot_bytes = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(snapshot_bytes.has_value());
        const auto profile_offset = bytes->size() - snapshot_bytes->size() - 8u - 88u - 4u - 64u;
        auto corrupt_profile = *bytes;
        corrupt_profile[profile_offset] ^= 1u;
        const auto rejected_profile = decodeWorldCheckpoint(corrupt_profile, *fixture.m_catalog);
        PPR_TEST_ASSERT(not rejected_profile and rejected_profile.error() == std::errc::invalid_argument);

        const auto incomplete = decodeWorldCheckpoint(
            std::span<const u8>{bytes->data(), profile_offset + 63u}, *fixture.m_catalog);
        PPR_TEST_ASSERT(not incomplete and incomplete.error() == sim::errc::truncated);

        auto restored = restoreWorldCheckpoint(fixture.m_catalog, *decoded, 92u);
        PPR_TEST_ASSERT(restored.has_value());
        PPR_DEFER {
            PPR_TEST_ASSERT(not (*restored)->shutdown());
        };

        const auto &current = *(*restored)->matterState();
        PPR_TEST_ASSERT(current.m_identity == MatterIdentity{92u, 7u, 4u});
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 22u>>(current.m_particles[0u]) == words);
        PPR_TEST_ASSERT((*restored)->update(TimeSpan{}) == std::errc::operation_not_permitted);

        const auto captured = captureWorldCheckpoint(**restored, (*restored)->preparedWorld()->generated());
        PPR_TEST_ASSERT(captured.has_value());
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 22u>>(captured->m_current_matter.m_particles[0u]) == words);

        for (u32 scenario = 0u; scenario < 8u; ++scenario) {
            auto bad = checkpoint;
            auto &invalid = bad.m_current_matter;
            switch (scenario) {
                case 0u: --invalid.m_profile.m_grid_width; break;
                case 1u: invalid.m_profile.m_delta_time *= 2.0f; break;
                case 2u: invalid.m_cell_metres *= 2.0; break;
                case 3u: invalid.m_particles[0u].m_mass *= 2.0f; break;
                case 4u: invalid.m_particles[0u].m_volume *= 2.0f; break;
                case 5u: invalid.m_particles[0u].m_material = 2.0f; break;
                case 6u:
                    invalid.m_particles[0u].m_deformation_gradient.m_c0y = std::numeric_limits<float>::quiet_NaN();
                    break;
                case 7u: invalid.m_particles.clear(); break;
            }

            PPR_TEST_ASSERT(validateWorldCheckpoint(bad, *fixture.m_catalog) == std::errc::invalid_argument);
        }

        std::fprintf(
            stderr, "[checkpoint-layout] WorldCheckpoint=%zu/%zu MatterState=%zu/%zu ColonyDriver=%zu/%zu\n",
            sizeof(WorldCheckpoint), alignof(WorldCheckpoint), sizeof(MatterState), alignof(MatterState),
            sizeof(ColonyDriver), alignof(ColonyDriver));
    };


    // ------------------------------------------------------------------
    // checkpoint envelope and frozen capture
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (v5_particle_wire_payload_is_the_exact_semantic_word_sequence) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        auto &particle = checkpoint.m_current_matter.m_particles[0u];
        particle.m_displacement_x = 0.125f;
        particle.m_displacement_y = -0.25f;
        particle.m_deformation_gradient = {1.0f, 0.25f, -0.125f, 0.75f};
        particle.m_deformation_displacement = {0.125f, 0.25f, -0.5f, -0.125f};
        particle.m_liquid_density = 0.5f;
        particle.m_lambda = 0.375f;
        particle.m_log_jp = 0.75f;
        particle.m_color_r = 0.25f;
        particle.m_color_g = -0.5f;
        particle.m_color_b = 0.375f;
        particle.m_enabled = 1.0f;

        const auto words = std::bit_cast<std::array<u32, 22u>>(particle);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));

        const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(encoded.has_value());
        const auto repeated = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(repeated.has_value() and *repeated == *encoded);

        // The carried matter block ends the envelope: profile, particle count,
        // the 22-word semantic payload, then the raw snapshot length and bytes.
        const auto snapshot_bytes = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(snapshot_bytes.has_value());
        const std::size_t payload_offset =
            encoded->size() - snapshot_bytes->size() - sizeof(u64) - words.size() * sizeof(u32) - sizeof(u32);
        PPR_TEST_ASSERT(
            payload_offset + 4u + words.size() * sizeof(u32) + 8u + snapshot_bytes->size() == encoded->size());
        PPR_TEST_ASSERT((*encoded)[payload_offset - 1u] == 0u and (*encoded)[payload_offset - 4u] == 1u);

        for (std::size_t index = 0u; index < words.size(); ++index) {
            u32 actual{};
            for (u32 byte = 0u; byte < sizeof(u32); ++byte) {
                actual |= static_cast<u32>((*encoded)[payload_offset + index * sizeof(u32) + byte]) << (byte * 8u);
            }

            PPR_TEST_ASSERT(actual == words[index]);
        }
    };

    PPR_UNIT_TEST (generator_seven_checkpoint_is_incompatible_before_material_id_restore) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(sim::kGeneratorAlgorithmVersion == 8u);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));

        checkpoint.m_descriptor.m_generator_version = 7u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);
        // Even an unknown saved cell ID cannot pass the identity boundary.
        checkpoint.m_snapshot.m_deltas.front().m_cells.front().m_element = 60000u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);
        const auto restored = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 1u);
        PPR_TEST_ASSERT(not restored and restored.error() == std::errc::protocol_not_supported);

        checkpoint.m_descriptor.m_generator_version = sim::kGeneratorAlgorithmVersion;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (ui_model_preserves_authored_metadata_and_nearest_agent_for_unresident_cells) {
        Fixture fixture;
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, editedCheckpoint(*fixture.m_catalog), 2u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;
        PPR_DEFER {
            std::ignore = driver.shutdown();
        };

        // Retain the authored recipe/layout and actor while terrain residency
        // is absent. Inspection must not fabricate a vacuum/temperature read.
        driver.grid() = sim::ChunkGrid{};
        const sim::GlobalCellPos target{30u, 45u};
        ColonyPresentation snapshot{};
        pP::game::ui::captureColonyPresentation(snapshot, driver, target, target);

        PPR_TEST_ASSERT(snapshot.m_inspected.m_presence == ECellPresence::unresident);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_material.empty());
        PPR_TEST_ASSERT(snapshot.m_inspected.m_temperature_k == 0.0f);
        PPR_TEST_ASSERT(driver.grid().residentChunkCount() == 0u);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_generated);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_biome == "Hearth");
        PPR_TEST_ASSERT(snapshot.m_inspected.m_nearest.has_value());
        PPR_TEST_ASSERT(snapshot.m_inspected.m_nearest->m_key == 100u);

        const sim::GlobalCellPos outside{sim::kWorldEdge, 45u};
        pP::game::ui::captureColonyPresentation(snapshot, driver, outside, outside);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_presence == ECellPresence::unavailable);
        PPR_TEST_ASSERT(not snapshot.m_inspected.m_generated and not snapshot.m_inspected.m_nearest);
        PPR_TEST_ASSERT(driver.grid().residentChunkCount() == 0u);
    };

    PPR_UNIT_TEST (round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        checkpoint.m_report.m_retained_known_payload_bytes = 123456u;
        checkpoint.m_report.m_known_phase_scratch_bytes.fill(654321u);

        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));

        const auto bytes = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(bytes.has_value());

        auto decoded = decodeWorldCheckpoint(*bytes, catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(decoded->m_report.m_retained_known_payload_bytes == 0u);
        PPR_TEST_ASSERT(std::ranges::all_of(decoded->m_report.m_known_phase_scratch_bytes, [](const u64 bytes) {
            return bytes == 0u;
        }));
        PPR_TEST_ASSERT(decoded->m_descriptor == checkpoint.m_descriptor);
        PPR_TEST_ASSERT(decoded->m_snapshot == checkpoint.m_snapshot);
        PPR_TEST_ASSERT(decoded->m_geometry.m_blobs == checkpoint.m_geometry.m_blobs);
        PPR_TEST_ASSERT(decoded->m_particles.m_next_id == 12u and decoded->m_particles.m_chunks.size() == 1u);
        PPR_TEST_ASSERT(
            decoded->m_particles.m_chunks[0].records()[0] == checkpoint.m_particles.m_chunks[0].records()[0]);

        const auto images = fixture.presentation(17u);
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, std::move(*decoded), 17u, {}, images);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;

        PPR_TEST_ASSERT(driver.preparedWorld()->generated().m_geometry.m_blobs == checkpoint.m_geometry.m_blobs);
        const auto presentation = makeSimFramePacket(1u, driver);
        PPR_TEST_ASSERT(presentation.has_value());

        for (const auto &material: catalog.materials()) {
            const auto found = std::ranges::find(
                presentation->m_terrain_visuals, material.m_numeric_id, &GridTerrainVisual::m_material_id);
            PPR_TEST_ASSERT(found != presentation->m_terrain_visuals.end());
            const u32 expected = (material.m_alpha_mode == EMaterialAlphaMode::blend ? kGridSurfaceOpticalBlend : 0u) |
                                 (material.m_texture_sampling == EMaterialTextureSampling::repeat
                                         ? kGridSurfaceTextureRepeat
                                         : 0u);
            PPR_TEST_ASSERT(found->m_surface_flags == expected);
        }

        PPR_TEST_ASSERT(driver.tickCount() == 19u and driver.worldGeneration() == 17u);
        PPR_TEST_ASSERT(driver.controlledEntity() == checkpoint.m_controlled);
        PPR_TEST_ASSERT(not driver.initialMatterBaselineValid());
        const auto particles = driver.captureParticles();
        PPR_TEST_ASSERT(particles.has_value());
        PPR_TEST_ASSERT(particles->m_next_id == 12u and particles->m_chunks.size() == 1u);
        PPR_TEST_ASSERT(particles->m_chunks[0].records()[0].m_id == 11u);
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == catalog.findMaterial("basalt")->m_numeric_id);
        PPR_TEST_ASSERT(driver.grid().getCell({128u, 40u})->m_temperature == 345.0f);

        const auto actor = *driver.controlledEntity();
        PPR_TEST_ASSERT(driver.registry().get<sim::BodyState>(actor)->m_velocity_x == 1.25f);
        PPR_TEST_ASSERT(driver.registry().get<Needs>(actor)->m_rest == 73.0f);
        PPR_TEST_ASSERT(driver.registry().get<Plan>(actor) == nullptr);

        driver.registry().emplace<Plan>(actor, {});
        driver.grid().clearAllDirty();
        PPR_TEST_ASSERT(not driver.grid().setCell({128u, 41u}, {catalog.findMaterial("basalt")->m_numeric_id, 310.0f}));
        const auto before = sim::capture(driver.grid(), 42u);

        ParallelScheduler chunks{"Checkpoint capture chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                // Capture/override-only fixtures must never admit a default generation.
                return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
            }};
        const auto address = restored->get();
        PPR_TEST_ASSERT(not job.requestCheckpoint(*restored));
        PPR_TEST_ASSERT(not *restored);
        PPR_TEST_ASSERT(not job.update(false));
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::queued);

        do {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        } while (job.view().m_state != EWorldJobState::prepared);

        PPR_TEST_ASSERT(not job.view().m_error);

        auto captured = job.takeCheckpoint();
        auto owner = job.takeCaptured();
        PPR_TEST_ASSERT(owner.get() == address);
        PPR_TEST_ASSERT(sim::capture(owner->grid(), 42u) == before);
        PPR_TEST_ASSERT(captured.m_snapshot.m_deltas.size() == 2u);
        sim::Registry scratch;
        PPR_TEST_ASSERT(not registerColonyComponents(scratch));
        PPR_TEST_ASSERT(not sim::restore(scratch, captured.m_snapshot));
        PPR_TEST_ASSERT(scratch.get<Plan>(actor) == nullptr);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(captured, catalog));

        while (job.busy()) {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        }

        PPR_TEST_ASSERT(not job.requestCheckpoint(owner));

        do {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        } while (job.view().m_state != EWorldJobState::prepared);

        PPR_TEST_ASSERT(not job.view().m_error);

        job.cancel(); // Completed but untaken payload must be disposed on coordinator.
        PPR_TEST_ASSERT(job.view().m_error == std::errc::operation_canceled);

        owner = job.takeCaptured();
        PPR_TEST_ASSERT(owner.get() == address);

        while (job.busy()) {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        }

        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
        PPR_TEST_ASSERT(sim::capture(owner->grid(), 42u) == before);

        PPR_TEST_ASSERT(not job.shutdown());
        PPR_TEST_ASSERT(not owner->shutdown());
    };

    PPR_UNIT_TEST (diagnostic_export_rejects_cancellation_and_unwritable_directory) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, editedCheckpoint(catalog), 2u);
        PPR_TEST_ASSERT(restored.has_value());
        const auto *const prepared = (*restored)->preparedWorld();
        PPR_TEST_ASSERT(prepared != nullptr);
        PPR_TEST_ASSERT(not prepared->presentation());

        auto [capture_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        PPR_TEST_ASSERT(
            exportWorldDiagnostics(catalog, prepared->generated(), prepared->descriptor(), {}, capture_context.get()) ==
            std::errc::operation_canceled);

        const auto path = fixture.m_root / "checkpoint-export-file-fixture";
        PPR_TEST_ASSERT(not std::filesystem::exists(path));
        PPR_DEFER {
            // Best-effort removal of a test-owned file, also on assertion unwind.
            std::error_code cleanup;
            std::filesystem::remove(path, cleanup);
        };

        {
            std::ofstream file{path, std::ios::binary};
            file << "This regular file cannot be an output directory.";
            PPR_TEST_ASSERT(file.good());
        }

        const auto error = exportWorldDiagnostics(catalog, prepared->generated(), prepared->descriptor(), path);
        PPR_TEST_ASSERT(error != std::error_code{});
        PPR_TEST_ASSERT(not (*restored)->shutdown());
    };

    PPR_UNIT_TEST (restored_current_matter_is_frozen_until_attached_and_keeps_physical_inventory) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, editedCheckpoint(catalog), 3u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;
        driver.setPaused(true);
        const auto frozen = driver.captureParticles();
        PPR_TEST_ASSERT(frozen.has_value());

        PPR_TEST_ASSERT(not driver.requestDig({127u, 40u}, 2u, 1u));
        PPR_TEST_ASSERT(driver.update(TimeSpan{}) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not driver.editPending());
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == catalog.findMaterial("basalt")->m_numeric_id);
        PPR_TEST_ASSERT(driver.particleCount() == 1u);
        const auto current = driver.captureParticles();
        PPR_TEST_ASSERT(current and current->m_chunks[0].m_particles == frozen->m_chunks[0].m_particles);

        PPR_TEST_ASSERT(driver.stepOne() == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(driver.tickCount() == 19u);
        PPR_TEST_ASSERT(driver.matterState() != nullptr and not driver.matterAttached());
        PPR_TEST_ASSERT(frozen->m_chunks[0].records()[0].m_id == 11u);

        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (checkpoint_blob_references_ranges_and_wire_counts_are_checked) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto original = editedCheckpoint(catalog);
        const auto plan = compileWorldPlan(catalog, catalog.worlds().front().m_key, 42u);
        PPR_TEST_ASSERT(plan.has_value());
        const auto &reference = original.m_geometry.m_blobs.front();
        const auto &deposit = plan->recipe().m_biomes[reference.m_biome].m_deposits[reference.m_deposit];

        for (u32 scenario = 0u; scenario < 13u; ++scenario) {
            auto bad = original;
            auto &blob = bad.m_geometry.m_blobs.front();
            switch (scenario) {
                case 0u: blob.m_biome = 10000u; break;
                case 1u: blob.m_deposit = 10000u; break;
                case 2u: blob.m_ordinal = 10000u; break;
                case 3u: blob.m_radius_m = 0.0; break;
                case 4u: blob.m_stretch = 100.0; break;
                case 5u: blob.m_angle = std::numeric_limits<double>::quiet_NaN(); break;
                case 6u: blob.m_x_m = -1.0; break;
                case 7u: blob.m_offset_y = 1025.0; break;
                case 8u: bad.m_geometry.m_blobs.push_back(blob); break;
                case 9u: bad.m_geometry.m_blobs.resize(4097u, blob); break;
                case 10u: blob.m_angle = -0.1; break;
                case 11u: blob.m_angle = 2.0 * std::numbers::pi + 0.1; break;
                case 12u:
                    blob.m_angle = 2.0 * std::numbers::pi * (deposit.m_orientation_max_degrees / 360.0) + 0.01;
                    break;
            }

            PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);
        }

        const auto encoded = encodeWorldCheckpoint(original, catalog);
        PPR_TEST_ASSERT(encoded.has_value());
        auto old_version = *encoded;
        old_version[8u] = 2u;
        const auto rejected_old = decodeWorldCheckpoint(old_version, catalog);
        PPR_TEST_ASSERT(not rejected_old and rejected_old.error() == std::errc::protocol_not_supported);

        std::array<u8, 16u> position{};
        const auto &first = original.m_geometry.m_blobs.front();
        for (u32 byte = 0u; byte < 8u; ++byte) {
            position[byte] = static_cast<u8>(std::bit_cast<u64>(first.m_x_m) >> (byte * 8u));
            position[8u + byte] = static_cast<u8>(std::bit_cast<u64>(first.m_y_m) >> (byte * 8u));
        }
        const auto record = std::ranges::search(*encoded, position);
        PPR_TEST_ASSERT(not record.empty());
        const auto offset = static_cast<std::size_t>(record.begin() - encoded->begin());
        PPR_TEST_ASSERT(offset >= 16u);

        auto excessive = *encoded;
        // Blob count precedes three u32 references and seven doubles.
        excessive[offset - 16u] = 1u;
        excessive[offset - 15u] = 16u;
        excessive[offset - 14u] = 0u;
        excessive[offset - 13u] = 0u;
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(excessive, catalog));

        auto truncated = *encoded;
        truncated.resize(offset + 8u);
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(truncated, catalog));
    };

    PPR_UNIT_TEST (malformed_identity_metric_counts_and_payload_are_rejected_before_restore) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto original = editedCheckpoint(catalog);
        auto bad = original;
        ++bad.m_descriptor.m_content_fingerprint;

        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::protocol_not_supported);

        bad = original;
        bad.m_descriptor.m_metric.m_cells_per_metre = 1u;
        PPR_TEST_ASSERT(not restoreWorldCheckpoint(fixture.m_catalog, std::move(bad), 2u));

        bad = original;
        ++bad.m_controlled->m_generation;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_materials.back().m_key = "wrong-key";
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::protocol_not_supported);

        bad = original;
        bad.m_snapshot.m_deltas[0].m_cells[0].m_element = 60000u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_initial_matter_valid = true;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_matter.m_regions[0].m_spans.push_back({40u, 128u, 130u});
        ++bad.m_matter.m_span_count;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_matter.m_regions[0].m_spans = {{41u, 127u, 129u}, {40u, 127u, 129u}};
        bad.m_matter.m_span_count = 2u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_particles.m_next_id = 11u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        auto duplicate = *bad.m_particles.m_chunks[0].m_particles;
        duplicate.push_back(duplicate[0]);
        bad.m_particles.m_chunks[0].m_particles = std::make_shared<const Array<sim::MatterParticle>>(
            std::move(duplicate));
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        for (u32 invalid = 0u; invalid < 6u; ++invalid) {
            bad = original;
            auto particles = *bad.m_particles.m_chunks[0].m_particles;
            if (invalid == 0u) {
                particles[0].m_x_m = std::numeric_limits<double>::quiet_NaN();
            }

            if (invalid == 1u) {
                particles[0].m_x_m = -1.0;
            }

            if (invalid == 2u) {
                particles[0].m_phase = sim::EGenerationMatter::gas;
            }

            if (invalid == 3u) {
                particles[0].m_bulk_density_kg_m3 = -1.0;
            }

            if (invalid == 4u) {
                particles[0].m_mass_kg = 10.0;
            }

            if (invalid == 5u) {
                particles[0].m_volume_m3 = 1.0;
                particles[0].m_mass_kg = 1.0;
            }

            bad.m_particles.m_chunks[0].m_particles = std::make_shared<const Array<sim::MatterParticle>>(
                std::move(particles));
            PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);
        }

        const auto encoded = encodeWorldCheckpoint(original, catalog);
        PPR_TEST_ASSERT(encoded.has_value());
        auto corrupt = *encoded;
        corrupt[8u] = 255u;
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        corrupt = *encoded;
        corrupt[12u] = 0u; // Explicit full-checkpoint discriminator.
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        // Locate this fixture's unique particle ID and continuous position in
        // the explicit wire record, then corrupt its preceding count field.
        std::array<u8, 16u> particle_prefix{};
        particle_prefix[0u] = 11u;
        const u64 x_bits = std::bit_cast<u64>(original.m_particles.m_chunks[0].records()[0].m_x_m);
        for (u32 byte = 0u; byte < 8u; ++byte) {
            particle_prefix[8u + byte] = static_cast<u8>(x_bits >> (byte * 8u));
        }
        const auto record = std::ranges::search(*encoded, particle_prefix);
        PPR_TEST_ASSERT(not record.empty());
        const auto record_offset = static_cast<std::size_t>(record.begin() - encoded->begin());
        PPR_TEST_ASSERT(record_offset >= 4u);

        corrupt = *encoded;
        const u32 oversized = sim::kMaxMatterParticles + 1u;
        for (u32 byte = 0u; byte < 4u; ++byte) {
            corrupt[record_offset - 4u + byte] = static_cast<u8>(oversized >> (byte * 8u));
        }
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        corrupt = *encoded;
        corrupt.push_back(0u);
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        for (const std::size_t size: std::array<std::size_t, 4u>{0u, 12u, 64u, encoded->size() - 1u}) {
            PPR_TEST_ASSERT(not decodeWorldCheckpoint(std::span<const u8>{encoded->data(), size}, catalog));
        }

        auto [capture_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled_decode = decodeWorldCheckpoint(*encoded, catalog, capture_context);
        PPR_TEST_ASSERT(not cancelled_decode and cancelled_decode.error() == std::errc::operation_canceled);
        const auto cancelled_encode = encodeWorldCheckpoint(original, catalog, capture_context);
        PPR_TEST_ASSERT(not cancelled_encode and cancelled_encode.error() == std::errc::operation_canceled);
        const auto cancelled_restore = restoreWorldCheckpoint(
            fixture.m_catalog, original, 4u, {.m_context = capture_context});
        PPR_TEST_ASSERT(not cancelled_restore and cancelled_restore.error() == std::errc::operation_canceled);

        PPR_TEST_ASSERT(not validateWorldCheckpoint(original, catalog));
    };

    // ------------------------------------------------------------------
    // named pack envelope and qualified profile wire contract
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (envelope_five_manifest_one_and_raw_snapshot_four_have_distinct_headers) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(kWorldCheckpointVersion == 5u and asset::Manifest::schema_version == 1u);
        PPR_TEST_ASSERT(sim::kSnapshotVersion == 4u and checkpoint.m_descriptor.m_pack_schema_version == 1u);

        const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(encoded.has_value());

        constexpr std::array<u8, 25u> header{
            0x50u, 0x50u, 0x52u, 0x57u, 0x47u, 0x45u, 0x4eu, 0x31u, 0x05u, 0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x00u,
            0x00u, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x13u, 0x00u, 0x00u, 0x00u};
        PPR_TEST_ASSERT(encoded->size() > header.size());
        PPR_TEST_ASSERT(std::ranges::equal(std::span{encoded->data(), header.size()}, header));
        constexpr std::string_view profile = "checkpoint:frontier";
        PPR_TEST_ASSERT(std::ranges::equal(std::span{encoded->data() + header.size(), profile.size()}, profile));

        const auto raw = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(raw.has_value());
        constexpr std::array<u8, 4u> raw_header{4u, 0u, 0u, 0u};
        PPR_TEST_ASSERT(std::ranges::equal(std::span{raw->data(), raw_header.size()}, raw_header));

        for (const u32 old_version: {0u, 1u, 2u, 3u, 4u}) {
            auto earlier = *encoded;
            earlier[8u] = static_cast<u8>(old_version);
            earlier.resize(12u);
            const auto rejected = decodeWorldCheckpoint(earlier, catalog);
            PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::protocol_not_supported);
        }

        auto flattened_schema = checkpoint;
        flattened_schema.m_descriptor.m_pack_schema_version = 3u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(flattened_schema, catalog) == std::errc::protocol_not_supported);

        auto wrong_magic = *encoded;
        wrong_magic.front() ^= 1u;
        const auto unknown = decodeWorldCheckpoint(wrong_magic, catalog);
        PPR_TEST_ASSERT(not unknown and unknown.error() == std::errc::protocol_not_supported);
    };

    PPR_UNIT_TEST (qualified_world_profiles_round_trip_at_the_authoritative_name_limit) {
        Fixture fixture{
            std::string(asset::QualifiedName::maximum_namespace_bytes, 'p'), std::string(config::kMaxKeyBytes, 'w')};
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(
            checkpoint.m_descriptor.m_profile.size() == asset::QualifiedName::reference_domain.m_max_length);

        const auto bytes = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(bytes.has_value());

        const auto decoded = decodeWorldCheckpoint(*bytes, catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(decoded->m_descriptor.m_profile == checkpoint.m_descriptor.m_profile);

        auto oversized = *bytes;
        const auto length = static_cast<u32>(asset::QualifiedName::reference_domain.m_max_length + 1u);
        for (u32 byte = 0u; byte < 4u; ++byte) {
            oversized[21u + byte] = static_cast<u8>(length >> (byte * 8u));
        }

        const auto rejected = decodeWorldCheckpoint(oversized, catalog);
        PPR_TEST_ASSERT(not rejected and rejected.error() == sim::errc::invalid_payload);

        checkpoint.m_descriptor.m_profile += 'w';
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);

        checkpoint.m_descriptor.m_profile = fixture.m_world_key;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);
    };

    // ------------------------------------------------------------------
    // selected runtime parameters and optional ready presentation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (runtime_generation_and_restore_reject_each_noncanonical_compile_parameter) {
        struct Clock final : ITimerClock {
            mutable u32 m_calls{};

            TimePoint now() const noexcept override {
                ++m_calls;
                return {};
            }
        } clock;

        Fixture fixture;
        std::array alternatives{
            kWorldCompileParameters, kWorldCompileParameters, kWorldCompileParameters, kWorldCompileParameters};
        alternatives[0].m_cell_size_m *= 1.5;
        alternatives[1].m_reference_thickness_m *= 2.0;
        alternatives[2].m_actor_half_width_m *= 0.75;
        alternatives[3].m_actor_half_height_m *= 0.75;
        config::Diagnostics diagnostics;

        for (const auto parameters: alternatives) {
            auto selected = selectWorldCatalog(
                fixture.m_catalog->descriptorOwner(), fixture.m_catalog->worldId(), parameters, diagnostics);
            PPR_TEST_ASSERT(selected.has_value());

            auto catalog = std::make_shared<const ContentCatalog>(std::move(*selected));
            auto checkpoint = editedCheckpoint(*catalog);

            auto plan = compileWorldPlan(*catalog, checkpoint.m_descriptor.m_profile, checkpoint.m_descriptor.m_seed);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->recipe().m_cell_size_m == parameters.m_cell_size_m);
            PPR_TEST_ASSERT(plan->recipe().m_reference_thickness_m == parameters.m_reference_thickness_m);
            PPR_TEST_ASSERT(plan->recipe().m_actor_half_width_m == parameters.m_actor_half_width_m);
            PPR_TEST_ASSERT(plan->recipe().m_actor_half_height_m == parameters.m_actor_half_height_m);

            const auto generated = prepareWorld(catalog, checkpoint.m_descriptor.m_profile, 42u, clock);
            PPR_TEST_ASSERT(not generated and generated.error() == std::errc::not_supported);
            PPR_TEST_ASSERT(clock.m_calls == 0u);
            PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, *catalog) == std::errc::not_supported);

            // Unsupported runtime parameters precede saved-layout and cell restoration.
            checkpoint.m_sites.clear();
            checkpoint.m_snapshot.m_deltas.front().m_cells.front().m_element = 60000u;
            const auto restored = restoreWorldCheckpoint(catalog, checkpoint, 1u);
            PPR_TEST_ASSERT(not restored and restored.error() == std::errc::not_supported);

            auto layout = sim::compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());

            sim::GeneratedWorld numeric{std::move(*plan), std::move(*layout), {}, {}, {}, {}, {}};
            const auto prepared = restorePreparedWorld(catalog, checkpoint.m_descriptor, std::move(numeric));
            PPR_TEST_ASSERT(not prepared and prepared.error() == std::errc::not_supported);
        }
    };

    PPR_UNIT_TEST (numeric_restore_omits_images_and_render_restore_requires_matching_owner_and_generation) {
        std::optional<SimFramePacket> retained;
        std::shared_ptr<const ContentPresentation> retained_images;

        {
            Fixture fixture;
            const auto checkpoint = editedCheckpoint(*fixture.m_catalog);
            auto numeric = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 5u);
            PPR_TEST_ASSERT(numeric.has_value());
            PPR_TEST_ASSERT(not (*numeric)->preparedWorld()->presentation());

            const auto unavailable = makeSimFramePacket(1u, **numeric);
            PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::invalid_argument);

            const auto uncaptured = captureWorldPresentation(**numeric);
            PPR_TEST_ASSERT(not uncaptured and uncaptured.error() == std::errc::invalid_argument);

            PPR_TEST_ASSERT(not (*numeric)->shutdown());

            const auto images = fixture.presentation(5u);
            const auto stale = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 6u, {}, images);
            PPR_TEST_ASSERT(not stale and stale.error() == std::errc::invalid_argument);

            const auto equivalent = std::make_shared<const ContentCatalog>(*fixture.m_catalog);
            const auto foreign_images = fixture.presentation(5u, equivalent);
            const auto foreign = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 5u, {}, foreign_images);
            PPR_TEST_ASSERT(not foreign and foreign.error() == std::errc::invalid_argument);

            auto ready = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 5u, {}, images);
            PPR_TEST_ASSERT(ready.has_value());
            PPR_TEST_ASSERT((*ready)->preparedWorld()->catalogOwner() == fixture.m_catalog);

            auto packet = makeSimFramePacket(2u, **ready);
            PPR_TEST_ASSERT(packet.has_value() and not packet->m_terrain_images.empty());
            PPR_TEST_ASSERT(packet->m_world_generation == 5u);

            retained.emplace(std::move(*packet));
            retained_images = images;
            PPR_TEST_ASSERT(not (*ready)->shutdown());
        }

        PPR_TEST_ASSERT(retained->m_terrain_images.front().m_storage.isOwned());
        PPR_TEST_ASSERT(not retained->m_terrain_images.front().m_storage.getBufferData().empty());
        PPR_TEST_ASSERT(retained_images->catalog()->worlds().front().m_key == "checkpoint:frontier");
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // checkpoint tests
    // ------------------------------------------------------------------

    const UnitTest checkpoint_tests = UnitTest::Named("checkpoint") / [](UnitTest::IRun &_) {
        using namespace detail::checkpoint;

        _.recurse(
            {generator_seven_checkpoint_is_incompatible_before_material_id_restore,
                diagnostic_export_rejects_cancellation_and_unwritable_directory,
                ui_model_preserves_authored_metadata_and_nearest_agent_for_unresident_cells,
                round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture,
                restored_current_matter_is_frozen_until_attached_and_keeps_physical_inventory,
                dig_candidate_preserves_carried_columns_and_emits_original_seam_inventory_once,
                new_contour_coverage_rejects_moving_and_gas_but_preserves_prior_overlap,
                v5_round_trip_preserves_all_current_words_and_rebinds_only_world,
                malformed_identity_metric_counts_and_payload_are_rejected_before_restore,
                checkpoint_blob_references_ranges_and_wire_counts_are_checked,
                envelope_five_manifest_one_and_raw_snapshot_four_have_distinct_headers,
                qualified_world_profiles_round_trip_at_the_authoritative_name_limit,
                runtime_generation_and_restore_reject_each_noncanonical_compile_parameter,
                numeric_restore_omits_images_and_render_restore_requires_matching_owner_and_generation});
    };

    const UnitTest &gameCheckpointTests() noexcept {
        return checkpoint_tests;
    }
}
