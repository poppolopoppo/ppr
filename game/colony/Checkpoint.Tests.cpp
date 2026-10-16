module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module game.tests;

import engine.asset;
import engine.core;
import engine.sim;
import engine.app;
import engine.image;
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
    // ------------------------------------------------------------------
    // explicit owned descriptor fixtures
    // ------------------------------------------------------------------

    using namespace colony;

    PPR_DEFINE_LOG_CATEGORY(CheckpointTests, debug, none)

    struct Fixture {
        std::filesystem::path m_root;
        std::string m_namespace, m_world_key, m_manifest;
        std::shared_ptr<const ContentCatalog> m_catalog;

        explicit Fixture(std::string name_space = "checkpoint", std::string world_key = "frontier")
            : m_namespace{std::move(name_space)}, m_world_key{std::move(world_key)},
              m_manifest{"schema_version = 1\npack_id = '" + m_namespace + "'\n"} {
            static std::atomic<u32> ordinal{};
            const auto parent = hal::process::currentExecutablePath().parent_path();

            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto candidate = parent / ("checkpoint-pack-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(candidate)) {
                    m_root = std::filesystem::canonical(candidate);
                    break;
                }
            }

            PPR_TEST_ASSERT(not m_root.empty());

            sim::asset::ElementDescriptor element;
            element.m_schema_version = 1u;
            element.m_density = 2000.0;
            element.m_phase = sim::EGenerationMatter::solid;
            add("solid", sim::asset::kElementKind, "solid.toml", element);

            element.m_density = 1.0;
            element.m_phase = sim::EGenerationMatter::liquid;
            add("liquid", sim::asset::kElementKind, "liquid.toml", element);

            image::TextureDescriptor texture;
            texture.m_schema_version = 1u;
            texture.m_source = "pixel.png";
            add("albedo", image::kTextureAssetKind, "albedo.toml", texture);

            MaterialAssetDescriptor material;
            material.m_schema_version = 1u;
            material.m_element = "solid";
            material.m_diggable = true;
            material.m_appearance.m_display_name = "Basalt";
            material.m_appearance.m_color_rgba = "808080ff";
            material.m_appearance.m_albedo_texture = "albedo";
            material.m_appearance.m_texture_period_m = 1.0;
            add("basalt", sim::asset::kMaterialKind, "basalt.toml", material);

            material.m_appearance.m_display_name = "Sandstone";
            material.m_appearance.m_alpha_mode = EMaterialAlphaMode::blend;
            material.m_appearance.m_texture_sampling = EMaterialTextureSampling::repeat;
            add("sandstone", sim::asset::kMaterialKind, "sandstone.toml", material);

            material.m_element = "liquid";
            material.m_diggable.reset();
            material.m_representation = sim::definition::EMaterialRepresentation::particle;
            material.m_appearance.m_display_name = "Water";
            add("water", sim::asset::kMaterialKind, "water.toml", material);

            sim::asset::FieldDescriptor field;
            field.m_schema_version = 1u;
            field.m_definition.m_frequency_per_metre = 0.01;
            add("noise", sim::asset::kFieldKind, "noise.toml", field);

            BiomeAssetDescriptor biome;
            biome.m_schema_version = 1u;
            biome.m_appearance.m_display_name = "Hearth";
            biome.m_appearance.m_preview_color_rgba = "808080ff";
            biome.m_definition.m_temperature = {{"noise"}, 300.0, 1.0};
            sim::definition::CompositionBand band;
            band.m_field.m_key = "noise";
            band.m_materials.m_entries.push_back({"stone", {{"basalt"}, 1.0}});
            biome.m_definition.m_composition.m_entries.push_back({"cover", std::move(band)});

            sim::definition::Deposit deposit;
            deposit.m_field.m_key = "noise";
            deposit.m_material.m_key = "sandstone";
            deposit.m_host_material.m_key = "basalt";
            deposit.m_blob.emplace();
            deposit.m_blob->m_count = 2u;
            deposit.m_blob->m_radius = {1.0, 2.0};
            deposit.m_blob->m_stretch = {1.0, 2.0};
            deposit.m_blob->m_orientation = {30.0, 180.0};
            biome.m_definition.m_deposits.m_entries.push_back({"lens", std::move(deposit)});
            add("hearth", sim::asset::kBiomeKind, "hearth.toml", biome);

            FeatureAssetDescriptor feature;
            feature.m_schema_version = 1u;
            feature.m_appearance.m_display_name = "Landing";
            feature.m_definition.m_reservation = {0, 0, 16, 16};
            sim::definition::Operation operation;
            operation.m_action = sim::definition::Carve{sim::definition::Rectangle{16u, 16u}};
            feature.m_definition.m_operations.m_entries.push_back({"clear", std::move(operation)});
            feature.m_definition.m_markers.m_entries.push_back({"spawn", {sim::EGenerationMarker::start, 8, 8}});
            add("landing", sim::asset::kFeatureKind, "landing.toml", feature);

            BackgroundAssetDescriptor background;
            background.m_schema_version = 1u;

            for (u32 index = 0u; index < 4u; ++index) {
                BackgroundAppearanceLayer layer;
                layer.m_order = index;
                layer.m_texture = "albedo";
                layer.m_color_rgba = "ffffffff";
                layer.m_repeat_period_m = 1.0;
                background.m_definition.m_layers.m_entries.push_back(
                    {"layer_" + std::to_string(index), std::move(layer)});
            }

            add("skies", kBackgroundAssetKind, "skies.toml", background);

            WorldAssetDescriptor world;
            world.m_schema_version = 1u;
            world.m_semantic_key = "frontier";
            world.m_appearance.m_display_name = "Frontier";
            world.m_appearance.m_background_stack = "skies";
            world.m_palette.m_materials.m_entries = {
                {"basalt", {"basalt", 1u}}, {"sandstone", {"sandstone", 2u}}, {"water", {"water", 3u}}};
            world.m_palette.m_fields.m_entries.push_back({"noise", {"noise", 0u}});
            world.m_palette.m_biomes.m_entries.push_back({"hearth", {"hearth", 0u}});
            world.m_palette.m_features.m_entries.push_back({"landing", {"landing", 0u}});
            world.m_profile.m_layout = {1u, 1u, 16u, 1.0};
            world.m_profile.m_boundaries = {{"basalt"}, 1u, 0.0, {"noise"}, {"noise"}};
            world.m_profile.m_start.m_biome.m_key = "hearth";
            world.m_profile.m_start.m_feature.m_key = "landing";
            world.m_profile.m_start.m_marker.m_key = "spawn";
            world.m_profile.m_start.m_x_fine = 128u;
            world.m_profile.m_start.m_y_fine = 128u;
            world.m_profile.m_start.m_clearance_m = 2.0;
            world.m_profile.m_start.m_route_width_m = 1.0;
            sim::definition::BiomeChoice choice;
            choice.m_biome.m_key = "hearth";
            choice.m_required_count = 1u;
            choice.m_distance_max_m = 1024.0;
            world.m_profile.m_biomes.m_entries.push_back({"hearth", std::move(choice)});
            add(m_world_key, sim::asset::kWorldKind, "world.toml", world);
            write("pack.toml", m_manifest);

            config::Diagnostics diagnostics;
            auto imported = loadContentCatalog(m_root, diagnostics);
            PPR_TEST_ASSERT(imported.has_value());

            auto definitions = std::make_shared<const ContentDescriptorCatalog>(std::move(*imported));
            const auto id = resolveContentWorld(*definitions, m_namespace + ":" + m_world_key, diagnostics);
            PPR_TEST_ASSERT(id.has_value());

            auto selected = selectWorldCatalog(definitions, *id, kWorldCompileParameters, diagnostics);
            PPR_TEST_ASSERT(selected.has_value());

            m_catalog = std::make_shared<const ContentCatalog>(std::move(*selected));
        }

        ~Fixture() {
            // Best-effort fixture cleanup; retained catalog and sidecar owners are independent of disk.
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
            PPR_LOG_WARNING_ON_FAIL(CheckpointTests, error);
        }

        Fixture(const Fixture &) = delete;
        Fixture &operator=(const Fixture &) = delete;

        void write(const std::string_view file, const std::string_view bytes) const {
            std::ofstream stream{m_root / file, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.close();
            PPR_TEST_ASSERT(not stream.fail());
        }

        template<typename T>
            requires requires { T::describe(); }
        void write(const std::string_view file, const T &value) const {
            const auto document = config::documentFrom(value);
            PPR_TEST_ASSERT(document.has_value());

            const auto bytes = config::encode(*document);
            PPR_TEST_ASSERT(bytes.has_value());

            write(file, std::string_view{*bytes});
        }

        template<typename T>
        void add(const std::string_view key, const std::string_view kind, const std::string_view file, const T &value) {
            m_manifest += "\n[assets." +
                          std::string{key} +
                          "]\nkind = '" +
                          std::string{kind} +
                          "'\ndescriptor = '" +
                          std::string{file} +
                          "'\n";

            write(file, value);
        }

        std::shared_ptr<const ContentPresentation> presentation(
            const u64 generation, std::shared_ptr<const ContentCatalog> catalog = {}) const {
            constexpr std::array<u8, 75u> pixel{
                0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u, 0x44u,
                0x52u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x04u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0xa9u,
                0xf1u, 0x9eu, 0x7eu, 0x00u, 0x00u, 0x00u, 0x12u, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9cu, 0x63u, 0x68u,
                0x68u, 0xf8u, 0xffu, 0x1fu, 0x19u, 0x33u, 0x90u, 0x2eu, 0x00u, 0x00u, 0x4cu, 0x7eu, 0x2fu, 0xe1u, 0x74u,
                0xdcu, 0xdau, 0x7cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u, 0x60u,
                0x82u};
            write("pixel.png", {reinterpret_cast<const char *>(pixel.data()), pixel.size()});

            config::Diagnostics diagnostics;
            auto request = ContentPresentationRequest::start(
                catalog ? std::move(catalog) : m_catalog, asset::RequestGeneration{generation}, context::background(),
                diagnostics);
            PPR_TEST_ASSERT(request.has_value());

            auto prepared = request->finish(diagnostics);
            PPR_TEST_ASSERT(prepared.has_value());
            return *prepared;
        }
    };

    // ------------------------------------------------------------------
    // edited numeric checkpoint values
    // ------------------------------------------------------------------

    // An explicitly edited world: its old authored starting area and reservoir
    // no longer have to satisfy generation-time containment or route guarantees.
    WorldCheckpoint editedCheckpoint(const ContentCatalog &catalog) {
        WorldCheckpoint checkpoint;
        checkpoint.m_descriptor.m_profile = catalog.worlds().front().m_key;
        checkpoint.m_descriptor.m_seed = 42u;
        checkpoint.m_descriptor.m_content_fingerprint = *catalog.contentFingerprint();
        checkpoint.m_materials.push_back({0u, "vacuum"});
        for (const auto &material: catalog.materials()) {
            checkpoint.m_materials.push_back({material.m_numeric_id, material.m_key});
        }

        std::ranges::sort(checkpoint.m_materials, {}, &CheckpointMaterial::m_id);

        auto plan = compileWorldPlan(catalog, checkpoint.m_descriptor.m_profile, 42u);
        PPR_TEST_ASSERT(plan.has_value());

        auto layout = sim::compileBiomeLayout(*plan);
        PPR_TEST_ASSERT(layout.has_value());
        checkpoint.m_sites.assign(layout->sites().begin(), layout->sites().end());

        for (u32 b = 0u; b < plan->recipe().m_biomes.size() and checkpoint.m_geometry.m_blobs.empty(); ++b) {
            const auto &deposits = plan->recipe().m_biomes[b].m_deposits;
            for (u32 d = 0u; d < deposits.size(); ++d) {
                const auto &deposit = deposits[d];
                if (deposit.m_shape == sim::EGenerationDepositShape::blob and deposit.m_blob_count >= 2u) {
                    checkpoint.m_geometry.m_blobs.push_back(
                        {b, d, 0u, 12.3456789, 45.125, deposit.m_radius_min_m, deposit.m_stretch_min,
                            2.0 * std::numbers::pi * (deposit.m_orientation_min_degrees / 360.0), 100.0, 200.0});
                    checkpoint.m_geometry.m_blobs.push_back(
                        {b, d, 1u, 90.25, 75.5, deposit.m_radius_max_m, deposit.m_stretch_max,
                            2.0 * std::numbers::pi * (deposit.m_orientation_max_degrees / 360.0), 500.0, 900.0});
                    break;
                }
            }
        }

        PPR_TEST_ASSERT(checkpoint.m_geometry.m_blobs.size() == 2u);

        checkpoint.m_start.m_resource_fine_cells.resize(plan->recipe().m_resources.size());
        checkpoint.m_start.m_spawn_x_m = 10.0;
        checkpoint.m_start.m_spawn_y_m = 15.0;
        checkpoint.m_start.m_floor_y_m = 14.0;
        checkpoint.m_initial_matter_valid = false;
        checkpoint.m_tick = 19u;
        checkpoint.m_report.m_material_counts.push_back({0u, 100u});

        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({127u, 40u}, {catalog.findMaterial("basalt")->m_numeric_id, 321.0f}));
        grid.clearAllDirty();
        PPR_TEST_ASSERT(not grid.setCell({128u, 40u}, {catalog.findMaterial("sandstone")->m_numeric_id, 345.0f}));

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
                .m_material = catalog.findMaterial("water")->m_numeric_id,
                .m_temperature_k = 293.0,
                .m_bulk_density_kg_m3 = 1.0,
                .m_volume_m3 = 0.1,
                .m_mass_kg = 0.1,
                .m_spans = {{40u, 127u, 129u}}});
        checkpoint.m_matter.m_span_count = 1u;

        checkpoint.m_particles.m_next_id = 12u;
        checkpoint.m_particles.m_chunks.push_back(
            {{1u, 0u}, std::make_shared<const Array<sim::MatterParticle>>(Array<sim::MatterParticle>{
                           {11u, 130.5 / 3.0, 40.5 / 3.0, 0.1, 0.1, 293.0, 1.0,
                               catalog.findMaterial("water")->m_numeric_id, sim::EGenerationMatter::liquid}})});
        return checkpoint;
    }

    // ------------------------------------------------------------------
    // checkpoint restore, frozen capture and edit publication
    // ------------------------------------------------------------------

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
        PPR_TEST_ASSERT(particles.m_next_id == 12u and particles.m_chunks.size() == 1u);
        PPR_TEST_ASSERT(particles.m_chunks[0].records()[0].m_id == 11u);
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

    PPR_UNIT_TEST (authored_particles_survive_headless_digs_and_wait_for_complete_build_publication) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, editedCheckpoint(catalog), 3u);
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
        const auto errand = driver.buildWall({{126u, 39u}, 9u, 3u, catalog.findMaterial("basalt")->m_numeric_id});
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

    PPR_UNIT_TEST (envelope_four_manifest_one_and_raw_snapshot_four_have_distinct_headers) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(kWorldCheckpointVersion == 4u and asset::Manifest::schema_version == 1u);
        PPR_TEST_ASSERT(sim::kSnapshotVersion == 4u and checkpoint.m_descriptor.m_pack_schema_version == 1u);

        const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(encoded.has_value());

        constexpr std::array<u8, 25u> header{
            0x50u, 0x50u, 0x52u, 0x57u, 0x47u, 0x45u, 0x4eu, 0x31u, 0x04u, 0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x00u,
            0x00u, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x13u, 0x00u, 0x00u, 0x00u};
        PPR_TEST_ASSERT(encoded->size() > header.size());
        PPR_TEST_ASSERT(std::ranges::equal(std::span{encoded->data(), header.size()}, header));
        constexpr std::string_view profile = "checkpoint:frontier";
        PPR_TEST_ASSERT(std::ranges::equal(std::span{encoded->data() + header.size(), profile.size()}, profile));

        const auto raw = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(raw.has_value());
        constexpr std::array<u8, 4u> raw_header{4u, 0u, 0u, 0u};
        PPR_TEST_ASSERT(std::ranges::equal(std::span{raw->data(), raw_header.size()}, raw_header));

        for (const u32 old_version: {0u, 1u, 2u, 3u}) {
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
                round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture,
                authored_particles_survive_headless_digs_and_wait_for_complete_build_publication,
                malformed_identity_metric_counts_and_payload_are_rejected_before_restore,
                checkpoint_blob_references_ranges_and_wire_counts_are_checked,
                envelope_four_manifest_one_and_raw_snapshot_four_have_distinct_headers,
                qualified_world_profiles_round_trip_at_the_authoritative_name_limit,
                runtime_generation_and_restore_reject_each_noncanonical_compile_parameter,
                numeric_restore_omits_images_and_render_restore_requires_matching_owner_and_generation});
    };

    const UnitTest &gameCheckpointTests() noexcept {
        return checkpoint_tests;
    }
}
