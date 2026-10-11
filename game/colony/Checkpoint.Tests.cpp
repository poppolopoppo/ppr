module;
#include "pP/UnitTest.h"
module game.tests;
import engine.core;
import engine.sim;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.checkpoint;
import game.colony.content;
import game.colony.driver;
import game.colony.worldgen;
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
        return checkpoint;
    }

    PPR_UNIT_TEST (round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture) {
        const auto catalog = loadCatalog();
        auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));
        const auto bytes = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(bytes.has_value());
        auto decoded = decodeWorldCheckpoint(*bytes, catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(decoded->m_descriptor == checkpoint.m_descriptor);
        PPR_TEST_ASSERT(decoded->m_snapshot == checkpoint.m_snapshot);
        auto restored = restoreWorldCheckpoint(loadCatalog(), std::move(*decoded), 17u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;
        PPR_TEST_ASSERT(driver.tickCount() == 19u and driver.worldGeneration() == 17u);
        PPR_TEST_ASSERT(driver.controlledEntity() == checkpoint.m_controlled);
        PPR_TEST_ASSERT(not driver.initialMatterBaselineValid());
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
        WorldJob job{chunks};
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
        const auto encoded = encodeWorldCheckpoint(original, catalog);
        PPR_TEST_ASSERT(encoded.has_value());
        auto corrupt = *encoded;
        corrupt[8u] = 255u;
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));
        corrupt = *encoded;
        corrupt[12u] = 0u; // Explicit full-checkpoint discriminator.
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
                malformed_identity_metric_counts_and_payload_are_rejected_before_restore});
    };
    const UnitTest &gameCheckpointTests() noexcept {
        return checkpoint_tests;
    }
}
