module;
#include "pP/UnitTest.h"
module game.tests;
import engine.core;
import engine.sim;
import engine.app;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.checkpoint;
import game.colony.content;
import game.colony.driver;
import game.colony.worldgen;
import game.colony.sim_packet;
import game.application;
import std;

namespace pP::game::tests::detail::checkpoint {
    using namespace colony;

    ContentCatalog loadCatalog() {
        config::Diagnostics diagnostics;
        auto catalog = loadContentCatalog(
            hal::process::currentExecutablePath().parent_path() / "worldgen/default", diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        return std::move(*catalog);
    }

    // An explicitly edited world: its old authored starting area and reservoir
    // no longer have to satisfy generation-time containment or route guarantees.
    WorldCheckpoint editedCheckpoint(const ContentCatalog &catalog) {
        WorldCheckpoint checkpoint;
        checkpoint.m_descriptor.m_profile = "frontier";
        checkpoint.m_descriptor.m_seed = 42u;
        checkpoint.m_descriptor.m_content_fingerprint = *catalog.contentFingerprint();
        checkpoint.m_materials.push_back({0u, "vacuum"});
        for (const auto &material: catalog.materials()) {
            checkpoint.m_materials.push_back({material.m_numeric_id, material.m_key});
        }
        std::ranges::sort(checkpoint.m_materials, {}, &CheckpointMaterial::m_id);
        auto plan = compileWorldPlan(catalog, "frontier", 42u);
        PPR_TEST_ASSERT(plan.has_value());
        auto layout = sim::compileBiomeLayout(*plan);
        PPR_TEST_ASSERT(layout.has_value());
        checkpoint.m_sites.assign(layout->sites().begin(), layout->sites().end());
        checkpoint.m_start.m_resource_fine_cells.resize(plan->recipe().m_resources.size());
        checkpoint.m_start.m_spawn_x_m = 10.0;
        checkpoint.m_start.m_spawn_y_m = 15.0;
        checkpoint.m_start.m_floor_y_m = 14.0;
        checkpoint.m_initial_matter_valid = false;
        checkpoint.m_tick = 19u;
        checkpoint.m_report.m_material_counts.push_back({0u, 100u});
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({127u, 40u}, {16u, 321.0f}));
        grid.clearAllDirty();
        PPR_TEST_ASSERT(not grid.setCell({128u, 40u}, {32u, 345.0f}));
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        const auto actor = registry.create();
        registry.emplace<Agent>(actor, {.m_key = 100u, .m_spawn = {30u, 45u}});
        registry.emplace<Needs>(actor, {85.0f, 73.0f});
        registry.emplace<sim::BodyState>(actor, {10.0f, 15.0f, 0.0f, 1.25f, -0.5f});
        registry.emplace<sim::BodyDefinition>(
            actor, {.m_motion = sim::EMotionKind::dynamic,
                       .m_half_width = 0.4f,
                       .m_half_height = 0.9f,
                       .m_fixed_rotation = true});
        auto snapshot = sim::captureFull(grid, registry, 42u);
        PPR_TEST_ASSERT(snapshot.has_value());
        checkpoint.m_snapshot = std::move(*snapshot);
        checkpoint.m_snapshot.m_bodies.m_replay_ticks = checkpoint.m_tick;
        checkpoint.m_controlled = actor;
        checkpoint.m_matter.m_regions.push_back(
            {.m_identity = 3u,
                .m_material = 1001u,
                .m_temperature_k = 293.0,
                .m_bulk_density_kg_m3 = 1.0,
                .m_volume_m3 = 0.1,
                .m_mass_kg = 0.1,
                .m_spans = {{40u, 127u, 129u}}});
        checkpoint.m_matter.m_span_count = 1u;
        checkpoint.m_particles.m_next_id = 12u;
        checkpoint.m_particles.m_chunks.push_back(
            {{1u, 0u},
                std::make_shared<const Array<sim::MatterParticle>>(Array<sim::MatterParticle>{
                    {11u, 130.5 / 3.0, 40.5 / 3.0, 0.1, 0.1, 293.0, 1.0, 1001u, sim::EGenerationMatter::liquid}})});
        return checkpoint;
    }

    PPR_UNIT_TEST (round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture) {
        const auto catalog = loadCatalog();
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
        PPR_TEST_ASSERT(decoded->m_particles.m_next_id == 12u and decoded->m_particles.m_chunks.size() == 1u);
        PPR_TEST_ASSERT(
            decoded->m_particles.m_chunks[0].records()[0] == checkpoint.m_particles.m_chunks[0].records()[0]);
        auto restored = restoreWorldCheckpoint(loadCatalog(), std::move(*decoded), 17u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;
        const auto presentation = makeSimFramePacket(1u, driver);
        for (const auto &material: catalog.materials()) {
            const auto found = std::ranges::find(
                presentation.m_terrain_visuals, material.m_numeric_id, &GridTerrainVisual::m_material_id);
            PPR_TEST_ASSERT(found != presentation.m_terrain_visuals.end());
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
        PPR_TEST_ASSERT(particles.m_next_id == 12u and particles.m_chunks.size() == 1u);
        PPR_TEST_ASSERT(particles.m_chunks[0].records()[0].m_id == 11u);
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == 16u);
        PPR_TEST_ASSERT(driver.grid().getCell({128u, 40u})->m_temperature == 345.0f);
        const auto actor = *driver.controlledEntity();
        PPR_TEST_ASSERT(driver.registry().get<sim::BodyState>(actor)->m_velocity_x == 1.25f);
        PPR_TEST_ASSERT(driver.registry().get<Needs>(actor)->m_rest == 73.0f);
        PPR_TEST_ASSERT(driver.registry().get<Plan>(actor) == nullptr);
        driver.registry().emplace<Plan>(actor, {});
        driver.grid().clearAllDirty();
        PPR_TEST_ASSERT(not driver.grid().setCell({128u, 41u}, {16u, 310.0f}));
        const auto before = sim::capture(driver.grid(), 42u);
        ParallelScheduler chunks{"Checkpoint capture chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{chunks, WorldJob::legacyFixturePrepare()};
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
        const auto catalog = loadCatalog();
        auto restored = restoreWorldCheckpoint(loadCatalog(), editedCheckpoint(catalog), 2u);
        PPR_TEST_ASSERT(restored.has_value());
        const auto *const prepared = (*restored)->preparedWorld();
        PPR_TEST_ASSERT(prepared != nullptr);
        auto [capture_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        PPR_TEST_ASSERT(
            exportWorldDiagnostics(catalog, prepared->generated(), prepared->descriptor(), {}, capture_context.get()) ==
            std::errc::operation_canceled);
        const auto path = hal::process::currentExecutablePath().parent_path() / "checkpoint-export-file-fixture";
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
        PPR_TEST_ASSERT(static_cast<bool>(error));
        PPR_TEST_ASSERT(not (*restored)->shutdown());
    };

    PPR_UNIT_TEST (authored_particles_survive_headless_digs_and_wait_for_complete_build_publication) {
        const auto catalog = loadCatalog();
        auto restored = restoreWorldCheckpoint(loadCatalog(), editedCheckpoint(catalog), 3u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;
        driver.setPaused(true);
        const auto frozen = driver.captureParticles();
        PPR_TEST_ASSERT(not driver.requestDig({127u, 40u}, 2u, 1u));
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(not driver.editPending());
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(driver.particleCount() == 1u);
        PPR_TEST_ASSERT(driver.captureParticles().m_chunks[0].m_particles == frozen.m_chunks[0].m_particles);

        driver.requireEditPublication();
        const auto errand = driver.buildWall({{126u, 39u}, 9u, 3u, 16u});
        PPR_TEST_ASSERT(errand.has_value());
        PPR_TEST_ASSERT(not driver.stepOne());
        for (u32 batch = 0u; batch < 32u and not driver.editReadyForPresentation(); ++batch) {
            PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        }
        PPR_TEST_ASSERT(driver.editReadyForPresentation());
        const auto body = *driver.registry().get<sim::BodyState>(*driver.controlledEntity());
        Array<ColonyDriver::DirtyChunk> manifest(driver.editManifest().begin(), driver.editManifest().end());
        PPR_TEST_ASSERT(manifest.size() == 2u);
        PPR_TEST_ASSERT(not driver.settleEdit({}));
        PPR_TEST_ASSERT(not driver.settleEdit(std::span<const ColonyDriver::DirtyChunk>{manifest.data(), 1u}));
        auto stale = manifest;
        --stale[0].m_revision;
        PPR_TEST_ASSERT(not driver.settleEdit(stale));
        PPR_TEST_ASSERT(driver.editPending() and driver.particleCount() == 1u);
        PPR_TEST_ASSERT(driver.grid().getCell({130u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(driver.registry().get<sim::BodyState>(*driver.controlledEntity())->m_y == body.m_y);
        PPR_TEST_ASSERT(driver.registry().get<BuildErrand>(*errand)->m_progress == 0u);
        PPR_TEST_ASSERT(not driver.settleEdit(manifest));
        PPR_TEST_ASSERT(not driver.editPending() and driver.particleCount() == 0u);
        PPR_TEST_ASSERT(driver.grid().getCell({130u, 40u})->m_element == driver.boundaryMaterial());
        PPR_TEST_ASSERT(frozen.m_chunks[0].records()[0].m_id == 11u);
        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (malformed_identity_metric_counts_and_payload_are_rejected_before_restore) {
        const auto catalog = loadCatalog();
        const auto original = editedCheckpoint(catalog);
        auto bad = original;
        ++bad.m_descriptor.m_content_fingerprint;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::protocol_not_supported);
        bad = original;
        bad.m_descriptor.m_metric.m_cells_per_metre = 1u;
        PPR_TEST_ASSERT(not restoreWorldCheckpoint(loadCatalog(), std::move(bad), 2u));
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
        PPR_TEST_ASSERT(
            decodeWorldCheckpoint(*encoded, catalog, capture_context.get()).error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(original, catalog));
    };
}

namespace pP::game::tests {
    const UnitTest checkpoint_tests = UnitTest::Named("checkpoint") / [](UnitTest::IRun &_) {
        using namespace detail::checkpoint;
        _.recurse(
            {diagnostic_export_rejects_cancellation_and_unwritable_directory,
                round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture,
                authored_particles_survive_headless_digs_and_wait_for_complete_build_publication,
                malformed_identity_metric_counts_and_payload_are_rejected_before_restore});
    };
    const UnitTest &gameCheckpointTests() noexcept {
        return checkpoint_tests;
    }
}
