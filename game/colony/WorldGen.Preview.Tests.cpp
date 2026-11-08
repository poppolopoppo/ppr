module;
#include "pP/UnitTest.h"
#include "pP/Macros.h"
module game.tests;
import engine.core;
import engine.sim;
import engine.asset;
import engine.app;
import game.colony.worldgen;
import game.colony.metric;
import game.colony.content;
import game.colony.driver;
import game.colony.agents;
import game.colony.sim_packet;
import game.colony.worldgen_preview;
import game.colony.worldgen_panel;
import imgui;
import imgui_internal;
import std;

namespace pP::game::tests::detail::worldgen_preview {
    // ------------------------------------------------------------------
    // world generation previews
    // ------------------------------------------------------------------

    using namespace colony;

    PPR_UNIT_TEST (lighting_showcase_has_catalog_backed_cells_and_canonical_actor_clearance) {
        const auto root = hal::process::currentExecutablePath().parent_path() / "worldgen/default";
        const std::array<std::filesystem::path, 1> roots{root};
        config::Diagnostics diagnostics;
        auto world = prepareLightingShowcase(roots, 42u, ITimerClock::steady(), diagnostics, {}, 1024u);
        PPR_TEST_ASSERT(world.has_value() and world->lightingShowcase());
        PPR_TEST_ASSERT(world->grid().edge() == 1024u and world->grid().residentChunkCount() == 1u);
        PPR_TEST_ASSERT(
            world->generated().m_matter.m_regions.empty() and world->generated().m_geometry.m_instances.empty());
        PPR_TEST_ASSERT(world->generated().m_plan.recipe().m_cell_size_m == kFineCellMetres);

        constexpr std::array<std::string_view, 12> keys{
            "sandstone", "sandstone", "clay", "iron_ore", "copper_ore", "showcase_amber_lamp",
            "showcase_violet_sculpture", "showcase_cyan_insert", "showcase_housing", "showcase_tread",
            "showcase_pipe_horizontal", "showcase_pipe_vertical"};
        std::array<u16, 12> ids{};
        for (u32 index = 0u; index < keys.size(); ++index) {
            const auto *const row = world->catalog().findMaterial(keys[index]);
            PPR_TEST_ASSERT(row != nullptr and row->m_representation == EMaterialRepresentation::terrain);
            ids[index] = row->m_numeric_id;

            if (index >= 8u) {
                PPR_TEST_ASSERT(row->m_numeric_id == 67u + index - 8u);
                PPR_TEST_ASSERT(row->m_texture_sampling == EMaterialTextureSampling::repeat);
                PPR_TEST_ASSERT(row->m_transport and row->m_transport->m_model == EMaterialOpticalModel::opaque);

                for (const double channel: row->m_emissive_rgb) {
                    PPR_TEST_ASSERT(channel == 0.0);
                }
            }
        }

        PPR_TEST_ASSERT(world->catalog().findMaterial(keys[5u])->m_emissive_strength == 2.5);
        PPR_TEST_ASSERT(world->catalog().findMaterial(keys[7u])->m_emissive_strength == 0.35);

        auto unique_ids = ids;
        std::ranges::sort(unique_ids);
        const auto duplicate_tail = std::ranges::unique(unique_ids);
        const auto expected_inventory = static_cast<std::size_t>(duplicate_tail.begin() - unique_ids.begin());

        const auto labels = lightingShowcaseLabels(ids);
        PPR_TEST_ASSERT(labels and labels->size() == kLightingSceneWidth * kLightingSceneHeight);
        for (u32 y = 0u; y < kLightingSceneHeight; ++y) {
            for (u32 x = 0u; x < kLightingSceneWidth; ++x) {
                const auto cell = world->grid().getCell({x, y});
                PPR_TEST_ASSERT(cell and cell->m_element == (*labels)[y * kLightingSceneWidth + x]);
                if (x == 0u or y == 0u or x + 1u == kLightingSceneWidth or y + 1u == kLightingSceneHeight) {
                    PPR_TEST_ASSERT(cell->m_element == 0u);
                }
            }
        }

        const auto exterior = world->grid().getCell({500u, 500u});
        PPR_TEST_ASSERT(exterior and exterior->m_element == 0u);

        // Only amber moves five native rows down beneath the ledge. Its four
        // cells retain their source value; cyan/violet keep every original label.
        const auto baseline = lightingSceneLabels(std::span<const u16>{ids.data(), 8u}, false, true);
        PPR_TEST_ASSERT(baseline.has_value());
        for (u32 index = 0u; index < baseline->size(); ++index) {
            if ((*baseline)[index] == ids[5u]) {
                PPR_TEST_ASSERT((*labels)[index] == 0u);
                PPR_TEST_ASSERT((*labels)[index - 5u * kLightingSceneWidth] == ids[5u]);
            } else if ((*baseline)[index] == ids[6u] or (*baseline)[index] == ids[7u]) {
                PPR_TEST_ASSERT((*labels)[index] == (*baseline)[index]);
            }
        }

        const auto at = [&](const u32 x, const u32 y) { return (*labels)[y * kLightingSceneWidth + x]; };

        PPR_TEST_ASSERT(std::ranges::count(*labels, ids[5u]) == 4);
        PPR_TEST_ASSERT(at(30u, 18u) == ids[8u] and at(31u, 18u) == ids[8u]);
        PPR_TEST_ASSERT(at(30u, 15u) == 0u and at(31u, 15u) == 0u);
        PPR_TEST_ASSERT(at(29u, 16u) == 0u and at(29u, 17u) == 0u);
        PPR_TEST_ASSERT(at(32u, 16u) == 0u and at(32u, 17u) == 0u);
        PPR_TEST_ASSERT(at(29u, 21u) == 0u and at(30u, 23u) == 0u);
        PPR_TEST_ASSERT(at(46u, 7u) == ids[8u] and at(43u, 12u) == ids[8u]);
        PPR_TEST_ASSERT(at(44u, 6u) == ids[8u] and at(51u, 6u) == ids[8u]);
        PPR_TEST_ASSERT(at(47u, 13u) == 0u and at(47u, 14u) == 0u and at(47u, 15u) == 0u);
        PPR_TEST_ASSERT(at(51u, 10u) == 0u and at(52u, 10u) == 0u and at(53u, 10u) == 0u);
        PPR_TEST_ASSERT(at(36u, 23u) == ids[10u] and at(39u, 20u) == ids[11u]);

        for (u32 y = 14u; y <= 17u; ++y) {
            PPR_TEST_ASSERT(at(36u, y) == ids[8u]);
        }

        PPR_TEST_ASSERT(at(26u, 5u) == ids[8u] and at(38u, 5u) == ids[8u]);
        PPR_TEST_ASSERT(at(8u, 10u) == ids[3u] and at(12u, 14u) == 0u);

        for (u32 x = 18u; x <= 41u; ++x) {
            PPR_TEST_ASSERT(at(x, 6u) == ids[9u]);
            for (u32 y = 7u; y <= 13u; ++y) {
                PPR_TEST_ASSERT(at(x, y) == 0u);
            }
        }

        u64 counted{};
        for (const auto &row: world->generated().m_report.m_material_counts) {
            counted += row.m_cells;
        }

        PPR_TEST_ASSERT(counted == 1024ull * 1024u);
        PPR_TEST_ASSERT(
            world->generated().m_report.m_grid_payload_bytes == u64(sim::kCellsPerChunk) * sizeof(sim::Cell));

        const auto start = validateLightingShowcaseStart(world->grid());
        PPR_TEST_ASSERT(
            start and
            start->m_spawn_x_m == world->generated().m_start.m_spawn_x_m and
            start->m_spawn_y_m == start->m_floor_y_m + kWorldCompileParameters.m_actor_half_height_m);

        // A real occupied canonical triangle in the actor body rejects the
        // authored start; no procedural landing guarantee or epsilon is used.
        const auto original = world->grid().getCell({19u, 9u});
        PPR_TEST_ASSERT(original.has_value());
        PPR_TEST_ASSERT(not world->grid().setCell({19u, 9u}, {ids[0], 293.15f}));
        const auto blocked = validateLightingShowcaseStart(world->grid());
        PPR_TEST_ASSERT(not blocked and blocked.error() == sim::make_error_code(sim::EGenerationErrc::start_invalid));

        PPR_TEST_ASSERT(not world->grid().setCell({19u, 9u}, *original));

        const auto missing = lightingSceneLabels(std::span<const u16>{ids.data(), 7u}, false, true);
        PPR_TEST_ASSERT(not missing and missing.error() == std::errc::invalid_argument);

        const auto incomplete_bay = lightingShowcaseLabels(std::span<const u16>{ids.data(), 11u});
        PPR_TEST_ASSERT(not incomplete_bay and incomplete_bay.error() == std::errc::invalid_argument);

        auto request = ContentPresentationRequest::start(
            world->catalogOwner(), asset::RequestGeneration{1u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        const auto images = request->finish(diagnostics);
        PPR_TEST_ASSERT(images.has_value());
        PPR_TEST_ASSERT(not world->attachPresentation(*images, asset::RequestGeneration{1u}));

        ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };

        PPR_TEST_ASSERT(not driver.initPrepared(
            {.m_seed = 42u, .m_agent_count = 1u, .m_world_generation = 1u, .m_world_edge = 1024u}, std::move(*world)));
        const auto stage = captureWorldPresentation(driver);
        PPR_TEST_ASSERT(
            stage.has_value() and
            stage->m_world_radiance_source and
            not stage->m_lighting_receivers and
            stage->m_reset_caches);
        PPR_TEST_ASSERT(stage->m_terrain_visuals.size() == expected_inventory);
        PPR_TEST_ASSERT(
            stage->m_world_radiance_source->width() == kLightingSceneWidth and
            stage->m_world_radiance_source->height() == kLightingSceneHeight and
            stage->m_world_radiance_revision == 1u and
            not stage->m_optical_geometry);
        PPR_TEST_ASSERT(stage->m_world_radiance_source->cellMetres() == double(stage->m_cell_size_m));

        const auto catalog_rows = captureCatalogPresentation(
            *driver.preparedWorld()->presentation(), driver.profileKey(), driver.boundaryMaterial(),
            driver.cellsPerMetre(), driver.referenceThicknessMetres());
        PPR_TEST_ASSERT(catalog_rows.has_value());
        PPR_TEST_ASSERT(std::ranges::any_of(catalog_rows->m_terrain_visuals, [](const GridTerrainVisual &visual) {
            return (visual.m_surface_flags & kGridSurfaceOpticalBlend) != 0u;
        }));

        for (const auto &visual: stage->m_terrain_visuals) {
            PPR_TEST_ASSERT(std::ranges::find(ids, visual.m_material_id) != ids.end());
            const auto source = std::ranges::find(
                catalog_rows->m_terrain_visuals, visual.m_material_id, &GridTerrainVisual::m_material_id);
            PPR_TEST_ASSERT(source != catalog_rows->m_terrain_visuals.end());
            PPR_TEST_ASSERT(
                visual.m_texture_index == source->m_texture_index and
                visual.m_normal_texture_index == source->m_normal_texture_index and
                visual.m_color_rgba == source->m_color_rgba and
                visual.m_roundness == source->m_roundness and
                visual.m_transport_model == source->m_transport_model and
                visual.m_emissive_strength == source->m_emissive_strength);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(
                    visual.m_emissive_rgb[channel] == source->m_emissive_rgb[channel] and
                    visual.m_sigma_per_m[channel] == source->m_sigma_per_m[channel]);
            }
        }

        GridPass spatial;
        PPR_DEFER {
            PPR_TEST_ASSERT(not spatial.shutdown());
        };

        PPR_TEST_ASSERT(not spatial.applyStagedFrame(*stage));
        PPR_TEST_ASSERT(not spatial.validateSpatialContent());

        // Use the actual prepared Driver/Scene and its canonical chunk colliders,
        // with the same full-size actor and normal PlayerCommand as the game.
        std::optional<sim::Entity> actor;
        for (const auto &[entity, agent]: driver.registry().view<Agent>()) {
            (void) agent;
            PPR_TEST_ASSERT(not actor);
            actor = entity;
        }

        PPR_TEST_ASSERT(actor.has_value());
        PPR_TEST_ASSERT(not driver.assignControl(actor));
        const auto *const body = driver.registry().get<sim::BodyDefinition>(*actor);
        PPR_TEST_ASSERT(body and body->m_half_width == 0.4f and body->m_half_height == 0.9f);
        PPR_TEST_ASSERT(driver.chainCount({0u, 0u}) > 0u);

        const auto tick = TimeSpan{std::chrono::seconds{1}} / 60u;
        const auto *const pose = driver.registry().get<sim::BodyState>(*actor);
        PPR_TEST_ASSERT(pose != nullptr);
        const float initial_x = pose->m_x;
        const float initial_y = pose->m_y;

        // One real Driver/Scene owns all states. Graphics ACKs are explicit
        // boundary inputs here; actual field/picture matching is covered by the
        // render fixture and the interactive acceptance sequence.
        const auto prepare_door = [&](const bool closed) {
            const auto revision = driver.requestServiceDoor(closed, driver.lightingRevision());
            PPR_TEST_ASSERT(revision.has_value());
            bool ready{};
            for (u32 attempt = 0u; attempt < 1024u and not ready; ++attempt) {
                const auto advanced = driver.advanceServiceDoor(*revision);
                PPR_TEST_ASSERT(advanced.has_value());
                ready = *advanced;
            }

            PPR_TEST_ASSERT(ready);
            return *revision;
        };

        const auto commit_door = [&](const u64 revision) {
            Array<ColonyDriver::DirtyChunk> manifest{driver.editManifest().begin(), driver.editManifest().end()};
            PPR_TEST_ASSERT(driver.settleEdit(manifest) == std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(not driver.markServiceDoorActivationSent(revision));
            PPR_TEST_ASSERT(not driver.acknowledgeServiceDoorActivation(revision, true));
            const auto committed = driver.commitServiceDoor(revision);
            PPR_TEST_ASSERT(committed and *committed and driver.lightingRevision() == revision);

            const auto frozen_tick = driver.tickCount();
            PPR_TEST_ASSERT(not driver.update(tick));
            PPR_TEST_ASSERT(driver.tickCount() == frozen_tick and driver.serviceDoorPublication());

            driver.ackPresentChunks(manifest);
            PPR_TEST_ASSERT(not driver.finishServiceDoor(revision));
        };

        const auto stale_request = driver.requestServiceDoor(true, driver.lightingRevision() + 1u);
        PPR_TEST_ASSERT(not stale_request and stale_request.error() == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not driver.serviceDoorPublication() and driver.grid().getCell({36u, 7u})->m_element == 0u);

        const auto canceled_revision = prepare_door(true);
        const auto candidate_stage = captureServiceDoorPresentation(driver, canceled_revision);
        PPR_TEST_ASSERT(candidate_stage and candidate_stage->m_world_radiance_revision == canceled_revision);
        PPR_TEST_ASSERT(not candidate_stage->m_gpu_particles and candidate_stage->m_particle_batches.empty());
        PPR_TEST_ASSERT(candidate_stage->m_uploads[0u].m_cell_materials[7u * sim::kChunkEdge + 36u] == ids[8u]);
        PPR_TEST_ASSERT(driver.grid().getCell({36u, 7u})->m_element == 0u);

        const auto canceled = driver.cancelServiceDoor(canceled_revision);
        PPR_TEST_ASSERT(canceled and *canceled and driver.lightingRevision() == 1u);
        PPR_TEST_ASSERT(not driver.serviceDoorPublication() and not driver.editPending());

        const auto closed_revision = prepare_door(true);
        PPR_TEST_ASSERT(closed_revision > canceled_revision);
        const auto stale_capture = captureServiceDoorPresentation(driver, canceled_revision);
        PPR_TEST_ASSERT(not stale_capture and stale_capture.error() == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(
            driver.acknowledgeServiceDoorActivation(canceled_revision, true) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(
            driver.serviceDoorPublication()->m_revision == closed_revision and not driver.serviceDoorClosed());

        commit_door(closed_revision);
        PPR_TEST_ASSERT(driver.serviceDoorClosed() and driver.grid().getCell({36u, 7u})->m_element == ids[8u]);
        PPR_TEST_ASSERT(not driver.setPlayerCommand({1, 0}));

        for (u32 step = 0u; step < 600u; ++step) {
            PPR_TEST_ASSERT(not driver.update(tick));
        }

        const float door_contact = 36.0f / static_cast<float>(driver.cellsPerMetre()) - body->m_half_width;
        PPR_TEST_ASSERT(pose->m_x >= door_contact - 0.05f and pose->m_x <= door_contact + 0.05f);

        driver.clearPlayerCommand();
        const auto open_revision = prepare_door(false);
        commit_door(open_revision);
        PPR_TEST_ASSERT(not driver.serviceDoorClosed() and driver.grid().getCell({36u, 7u})->m_element == 0u);
        bool rejected_actor_overlap{};

        PPR_TEST_ASSERT(not driver.setPlayerCommand({1, 0}));

        for (u32 step = 0u; step < 600u and pose->m_x < 38.5f / 3.0f; ++step) {
            PPR_TEST_ASSERT(not driver.update(tick));
            if (not rejected_actor_overlap and
                pose->m_x + body->m_half_width > 36.0f / 3.0f and
                pose->m_x - body->m_half_width < 37.0f / 3.0f) {
                const auto rejected = driver.requestServiceDoor(true, driver.lightingRevision());
                PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::device_or_resource_busy);
                PPR_TEST_ASSERT(not driver.serviceDoorPublication() and driver.lightingRevision() == open_revision);
                rejected_actor_overlap = true;
            }

            PPR_TEST_ASSERT(std::isfinite(pose->m_x) and std::isfinite(pose->m_y));
            PPR_TEST_ASSERT(std::abs(pose->m_y - initial_y) < 0.2f);
        }

        PPR_TEST_ASSERT(rejected_actor_overlap);
        PPR_TEST_ASSERT(pose->m_x >= 38.5f / 3.0f and pose->m_x > initial_x + 5.0f);

        // Continuing toward the visibly occupied tank must not pass through its
        // left shell. No teleported actor, synthetic blocker or jump is used.
        for (u32 step = 0u; step < 240u; ++step) {
            PPR_TEST_ASSERT(not driver.update(tick));
        }

        // The straight canonical left face is at native x43. Require actual
        // approach to that face using the real box half-width, not merely any
        // blocked position beyond the earlier route milestone. The 5 cm allowance
        // covers contour contact/solver skin and is far below one native cell
        // (33 cm), or the >1 m gap from that milestone to this contact position.
        const float tank_face_x = 43.0f / static_cast<float>(driver.cellsPerMetre());
        const float contact_x = tank_face_x - body->m_half_width;
        constexpr float contact_margin = 0.05f;
        PPR_TEST_ASSERT(pose->m_x >= contact_x - contact_margin and pose->m_x <= contact_x + contact_margin);
        PPR_TEST_ASSERT(pose->m_y > 1.0f and std::isfinite(pose->m_y));

        // Keep the same rightward input active after reaching the shell. A
        // premature snag cannot satisfy the approach bound; penetrating or
        // drifting through the actual tank cannot satisfy these stable samples.
        const float reached_x = pose->m_x;
        PPR_TEST_ASSERT(not driver.setPlayerCommand({1, 0}));

        for (u32 step = 0u; step < 12u; ++step) {
            PPR_TEST_ASSERT(not driver.update(tick));
            PPR_TEST_ASSERT(pose->m_x >= contact_x - contact_margin and pose->m_x <= contact_x + contact_margin);
            PPR_TEST_ASSERT(std::abs(pose->m_x - reached_x) <= contact_margin);
        }

        driver.clearPlayerCommand();
        const auto reclosed_revision = prepare_door(true);
        commit_door(reclosed_revision);
        PPR_TEST_ASSERT(driver.serviceDoorClosed() and reclosed_revision > open_revision);

        // The mounted lamp changes only its source-state projection. All real
        // cells/colliders, other source rows and shared image owners survive.
        const auto lamp_before = captureWorldPresentation(driver);
        PPR_TEST_ASSERT(lamp_before and driver.serviceLampEnabled());
        const auto lamp_collider_count = driver.chainCount({0u, 0u});

        const auto prepare_lamp = [&](const bool enabled) {
            const auto revision = driver.requestServiceLamp(enabled, driver.lightingRevision());
            PPR_TEST_ASSERT(revision.has_value());

            bool ready{};
            for (u32 attempt = 0u; attempt < 1024u and not ready; ++attempt) {
                const auto advanced = driver.advanceServiceDoor(*revision);
                PPR_TEST_ASSERT(advanced.has_value());
                ready = *advanced;
            }

            PPR_TEST_ASSERT(
                ready and driver.serviceDoorPublication()->m_target == ColonyDriver::EServiceBayTarget::amber_lamp);
            return *revision;
        };

        const auto check_lamp_stage = [&](const GridStagedFrame &actual, const bool enabled) {
            PPR_TEST_ASSERT(actual.m_terrain_visuals.size() == lamp_before->m_terrain_visuals.size());
            PPR_TEST_ASSERT(actual.m_terrain_images.size() == lamp_before->m_terrain_images.size());
            PPR_TEST_ASSERT(actual.m_uploads.size() == lamp_before->m_uploads.size());

            for (u32 index = 0u; index < actual.m_uploads.size(); ++index) {
                PPR_TEST_ASSERT(
                    std::ranges::equal(
                        actual.m_uploads[index].m_cell_materials, lamp_before->m_uploads[index].m_cell_materials));
            }

            for (u32 index = 0u; index < actual.m_terrain_visuals.size(); ++index) {
                auto expected = lamp_before->m_terrain_visuals[index];
                if (expected.m_material_id == ids[5u]) {
                    expected.m_emissive_strength = enabled ? 2.5f : 0.0f;
                }

                // GridTerrainVisual is the existing all-scalar GPU protocol;
                // this compares every property, including cyan/violet source S.
                PPR_TEST_ASSERT(std::memcmp(&actual.m_terrain_visuals[index], &expected, sizeof(expected)) == 0);

                const auto optical = gridMaterialOpticalCell(actual.m_terrain_visuals[index]);
                PPR_TEST_ASSERT(optical.has_value());
                PPR_TEST_ASSERT(
                    not actual.m_world_radiance_source->validateMaterialProjection(expected.m_material_id, *optical));
            }

            for (u32 index = 0u; index < actual.m_terrain_images.size(); ++index) {
                const auto &a = actual.m_terrain_images[index];
                const auto &b = lamp_before->m_terrain_images[index];
                PPR_TEST_ASSERT(a.m_storage == b.m_storage and a.m_width == b.m_width and a.m_height == b.m_height);
                PPR_TEST_ASSERT(a.m_format == b.m_format and a.m_mip_count == b.m_mip_count);
                PPR_TEST_ASSERT(a.m_subresources.size() == b.m_subresources.size());

                for (u32 mip = 0u; mip < a.m_subresources.size(); ++mip) {
                    const auto &x = a.m_subresources[mip];
                    const auto &y = b.m_subresources[mip];
                    PPR_TEST_ASSERT(
                        x.m_view == y.m_view and x.m_row_pitch == y.m_row_pitch and x.m_slice_pitch == y.m_slice_pitch);
                }
            }

            PPR_TEST_ASSERT(
                std::ranges::equal(
                    actual.m_world_radiance_source->materialIds(),
                    lamp_before->m_world_radiance_source->materialIds()));
        };

        const auto canceled_lamp = prepare_lamp(false);
        const auto off_candidate = captureServiceDoorPresentation(driver, canceled_lamp);
        PPR_TEST_ASSERT(off_candidate.has_value());
        check_lamp_stage(*off_candidate, false);
        PPR_TEST_ASSERT(driver.serviceLampEnabled());

        const auto canceled_lamp_result = driver.cancelServiceDoor(canceled_lamp);
        PPR_TEST_ASSERT(canceled_lamp_result and *canceled_lamp_result and driver.serviceLampEnabled());
        PPR_TEST_ASSERT(driver.lightingRevision() == reclosed_revision);

        const auto off_revision = prepare_lamp(false);
        PPR_TEST_ASSERT(off_revision > canceled_lamp);
        PPR_TEST_ASSERT(
            driver.acknowledgeServiceDoorActivation(canceled_lamp, true) == std::errc::operation_not_permitted);

        commit_door(off_revision);
        PPR_TEST_ASSERT(not driver.serviceLampEnabled() and driver.serviceDoorClosed());
        PPR_TEST_ASSERT(driver.chainCount({0u, 0u}) == lamp_collider_count);

        const auto off_committed = captureWorldPresentation(driver);
        PPR_TEST_ASSERT(off_committed.has_value());
        check_lamp_stage(*off_committed, false);

        const auto on_revision = prepare_lamp(true);

        commit_door(on_revision);

        const auto on_committed = captureWorldPresentation(driver);
        PPR_TEST_ASSERT(on_committed and driver.serviceLampEnabled() and driver.serviceDoorClosed());
        PPR_TEST_ASSERT(driver.chainCount({0u, 0u}) == lamp_collider_count);
        check_lamp_stage(*on_committed, true);
        PPR_TEST_ASSERT(driver.contentCatalog()->findMaterial(keys[5u])->m_emissive_strength == 2.5);

        // Relocation swaps complete native cells within the same publication.
        // Unequal temperatures make a material-only copy observably incorrect.
        using LampPosition = ColonyDriver::EServiceLampPosition;
        PPR_TEST_ASSERT(driver.serviceLampPosition() == LampPosition::original);
        std::array<sim::Cell, 4u> lamp_cells{};
        std::array<sim::Cell, 4u> vacuum_cells{};
        for (u32 index = 0u; index < 4u; ++index) {
            const u32 y = 16u + index / 2u;
            const u32 dx = index % 2u;
            lamp_cells[index] = {ids[5u], 310.0f + float(index)};
            vacuum_cells[index] = {0u, 270.0f + float(index)};
            PPR_TEST_ASSERT(not driver.grid().setCell({30u + dx, y}, lamp_cells[index]));
            PPR_TEST_ASSERT(not driver.grid().setCell({37u + dx, y}, vacuum_cells[index]));
        }

        const auto reject_move = [&](const LampPosition position, const u64 revision, const std::errc error) {
            const auto rejected = driver.requestServiceLampPosition(position, revision);
            PPR_TEST_ASSERT(not rejected and rejected.error() == error);
        };

        reject_move(static_cast<LampPosition>(255u), driver.lightingRevision(), std::errc::invalid_argument);
        reject_move(LampPosition::original, driver.lightingRevision(), std::errc::operation_not_permitted);
        reject_move(LampPosition::alternate, driver.lightingRevision() + 1u, std::errc::operation_not_permitted);

        const auto prepare_move = [&](const LampPosition position) {
            const auto revision = driver.requestServiceLampPosition(position, driver.lightingRevision());
            PPR_TEST_ASSERT(revision.has_value());

            bool ready{};
            for (u32 attempt = 0u; attempt < 1024u and not ready; ++attempt) {
                const auto advanced = driver.advanceServiceDoor(*revision);
                PPR_TEST_ASSERT(advanced.has_value());
                ready = *advanced;
            }

            PPR_TEST_ASSERT(ready);
            PPR_TEST_ASSERT(
                driver.serviceDoorPublication()->m_target == ColonyDriver::EServiceBayTarget::amber_lamp_position);
            return *revision;
        };

        const auto check_move_cells = [&](const bool alternate) {
            for (u32 index = 0u; index < 4u; ++index) {
                const u32 y = 16u + index / 2u;
                const u32 dx = index % 2u;
                const auto left = driver.grid().getCell({30u + dx, y});
                const auto right = driver.grid().getCell({37u + dx, y});
                const auto &expected_left = alternate ? vacuum_cells[index] : lamp_cells[index];
                const auto &expected_right = alternate ? lamp_cells[index] : vacuum_cells[index];
                PPR_TEST_ASSERT(left and right);
                PPR_TEST_ASSERT(
                    left->m_element == expected_left.m_element and left->m_temperature == expected_left.m_temperature);
                PPR_TEST_ASSERT(
                    right->m_element == expected_right.m_element and
                    right->m_temperature == expected_right.m_temperature);
            }
        };

        const auto canceled_move = prepare_move(LampPosition::alternate);
        const auto moved_candidate = captureServiceDoorPresentation(driver, canceled_move);
        PPR_TEST_ASSERT(moved_candidate and moved_candidate->m_world_radiance_revision == canceled_move);
        const auto candidate_labels = moved_candidate->m_world_radiance_source->materialIds();
        for (u32 y = 16u; y <= 17u; ++y) {
            for (u32 dx = 0u; dx < 2u; ++dx) {
                PPR_TEST_ASSERT(candidate_labels[y * kLightingSceneWidth + 30u + dx] == 0u);
                PPR_TEST_ASSERT(candidate_labels[y * kLightingSceneWidth + 37u + dx] == ids[5u]);
                PPR_TEST_ASSERT(moved_candidate->m_uploads[0u].m_cell_materials[y * sim::kChunkEdge + 30u + dx] == 0u);
                PPR_TEST_ASSERT(
                    moved_candidate->m_uploads[0u].m_cell_materials[y * sim::kChunkEdge + 37u + dx] == ids[5u]);
            }
        }

        PPR_TEST_ASSERT(std::ranges::count(candidate_labels, ids[5u]) == 4);
        check_move_cells(false);
        PPR_TEST_ASSERT(driver.serviceLampPosition() == LampPosition::original);
        reject_move(LampPosition::alternate, driver.lightingRevision(), std::errc::device_or_resource_busy);
        const auto move_canceled = driver.cancelServiceDoor(canceled_move);
        PPR_TEST_ASSERT(move_canceled and *move_canceled);
        check_move_cells(false);

        const auto forward_move = prepare_move(LampPosition::alternate);
        PPR_TEST_ASSERT(forward_move > canceled_move);
        PPR_TEST_ASSERT(
            driver.acknowledgeServiceDoorActivation(canceled_move, true) == std::errc::operation_not_permitted);
        const auto premature_commit = driver.commitServiceDoor(forward_move);
        PPR_TEST_ASSERT(not premature_commit and premature_commit.error() == std::errc::operation_not_permitted);
        check_move_cells(false);

        commit_door(forward_move);
        PPR_TEST_ASSERT(driver.serviceLampPosition() == LampPosition::alternate and driver.serviceLampEnabled());
        check_move_cells(true);
        const auto relocated_stage = captureWorldPresentation(driver);
        PPR_TEST_ASSERT(relocated_stage and relocated_stage->m_world_radiance_revision == forward_move);
        PPR_TEST_ASSERT(std::ranges::equal(relocated_stage->m_world_radiance_source->materialIds(), candidate_labels));

        const auto alternate_off = prepare_lamp(false);
        commit_door(alternate_off);
        const auto alternate_off_stage = captureWorldPresentation(driver);
        PPR_TEST_ASSERT(alternate_off_stage.has_value());
        const auto alternate_off_visual = std::ranges::find(
            alternate_off_stage->m_terrain_visuals, u32(ids[5u]), &GridTerrainVisual::m_material_id);
        PPR_TEST_ASSERT(
            alternate_off_visual != alternate_off_stage->m_terrain_visuals.end() and
            alternate_off_visual->m_emissive_strength == 0.0f);

        const auto intervening_door = prepare_door(false);
        commit_door(intervening_door);
        PPR_TEST_ASSERT(driver.serviceLampPosition() == LampPosition::alternate and not driver.serviceLampEnabled());

        const auto reverse_move = prepare_move(LampPosition::original);
        commit_door(reverse_move);
        PPR_TEST_ASSERT(driver.serviceLampPosition() == LampPosition::original and not driver.serviceLampEnabled());
        check_move_cells(false);
        const auto restored_on = prepare_lamp(true);
        commit_door(restored_on);
        PPR_TEST_ASSERT(driver.serviceLampEnabled());

        // Destination occupancy rejects before exposing any candidate.
        PPR_TEST_ASSERT(not driver.grid().setCell({37u, 16u}, {ids[8u], 280.0f}));
        const auto occupied_move = driver.requestServiceLampPosition(
            LampPosition::alternate, driver.lightingRevision());
        PPR_TEST_ASSERT(not occupied_move and occupied_move.error() == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not driver.serviceDoorPublication() and driver.serviceLampPosition() == LampPosition::original);
        PPR_TEST_ASSERT(not driver.grid().setCell({37u, 16u}, vacuum_cells[0u]));

        auto *const mutable_pose = driver.registry().get<sim::BodyState>(*actor);
        const auto saved_pose = *mutable_pose;
        mutable_pose->m_x = 37.5f / static_cast<float>(driver.cellsPerMetre());
        mutable_pose->m_y = 16.5f / static_cast<float>(driver.cellsPerMetre());
        const auto overlap_move = driver.requestServiceLampPosition(LampPosition::alternate, driver.lightingRevision());
        PPR_TEST_ASSERT(not overlap_move and overlap_move.error() == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not driver.serviceDoorPublication() and driver.serviceLampPosition() == LampPosition::original);
        *mutable_pose = saved_pose;

        // The dedicated-row admission must reject a fifth lamp occurrence;
        // failed preparation leaves the committed source state/revision alone.
        const auto extra = driver.grid().getCell({40u, 20u});
        PPR_TEST_ASSERT(extra.has_value());
        PPR_TEST_ASSERT(not driver.grid().setCell({40u, 20u}, {ids[5u], extra->m_temperature}));
        const auto duplicate_move = driver.requestServiceLampPosition(
            LampPosition::alternate, driver.lightingRevision());
        PPR_TEST_ASSERT(not duplicate_move and duplicate_move.error() == std::errc::not_supported);

        const auto rejected_lamp = driver.requestServiceLamp(false, driver.lightingRevision());
        PPR_TEST_ASSERT(not rejected_lamp and rejected_lamp.error() == std::errc::not_supported);
        PPR_TEST_ASSERT(
            not driver.serviceDoorPublication() and
            driver.serviceLampEnabled() and
            driver.lightingRevision() == restored_on);
        PPR_TEST_ASSERT(not driver.grid().setCell({40u, 20u}, *extra));
    };

    PPR_UNIT_TEST (workshop_widgets_block_invalid_actions_and_focus_nested_property_errors) {
        auto *const previous = ImGui::GetCurrentContext();
        auto *const context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        PPR_DEFER {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        };

        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1400.0f, 1000.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        WorldgenPanelState state;
        WorldgenPanelView view;
        const auto frame = [&](const ImVec2 position, const bool down) {
            io.AddMousePosEvent(position.x, position.y);
            io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame();
            auto intents = drawWorldgenPanel(state, view);
            ImGui::Render();
            return intents;
        };

        const auto emitted = [](const Array<WorkshopIntent> &intents, const EWorkshopIntent kind) {
            return std::ranges::any_of(intents, [&](const WorkshopIntent &intent) { return intent.m_kind == kind; });
        };

        static_cast<void>(frame(ImVec2{-1.0f, -1.0f}, false));
        auto *const window = ImGui::FindWindowByName("Authoring Workspace###Worldgen Workshop");
        PPR_TEST_ASSERT(window != nullptr);

        // Internals locate a submitted item's navigation rectangle; real mouse
        // down/up frames then drive the same Button, MenuItem and BeginMenu the
        // pointer would.
        const auto itemCenter = [&](ImGuiWindow *const target, const ImGuiID id, const ImGuiNavLayer layer) {
            PPR_TEST_ASSERT(target != nullptr);

            // Settle native navigation first so the previous pointer cannot
            // override the requested navigation target.
            static_cast<void>(frame(ImVec2{-100.0f, -100.0f}, false));
            ImGui::FocusWindow(target);
            ImGui::SetNavCursorVisible(true);
            ImGui::SetNavID(id, layer, 0u, ImRect{});
            static_cast<void>(frame(ImVec2{-100.0f, -100.0f}, false));

            const ImRect rect = target->NavRectRel[layer];
            PPR_TEST_ASSERT(rect.GetWidth() > 0.0f and rect.GetHeight() > 0.0f);
            return ImGui::WindowRectRelToAbs(target, rect).GetCenter();
        };

        const auto click = [&](const ImVec2 position) {
            static_cast<void>(frame(position, false));
            static_cast<void>(frame(position, true));
            return frame(position, false);
        };

        const auto clickItem = [&](ImGuiWindow *const target, const ImGuiID id, const ImGuiNavLayer layer) {
            return click(itemCenter(target, id, layer));
        };

        // "Load pack" carries the open intent and stays disabled while the pack
        // root is blank.
        const auto load_pack = [&]() { return clickItem(window, window->GetID("Load pack"), ImGuiNavLayer_Main); };

        PPR_TEST_ASSERT(not emitted(load_pack(), EWorkshopIntent::open));
        std::snprintf(state.m_root.data(), state.m_root.size(), "%s", "authored-pack");
        PPR_TEST_ASSERT(emitted(load_pack(), EWorkshopIntent::open));

        const auto root = std::filesystem::canonical(hal::process::currentExecutablePath().parent_path());
        ContentSourceSet source;
        source.m_packs.push_back(
            {root,
                {root / "pack.toml",
                    "schema_version = 1\npack_id = 'widget'\n[assets.hearth]\nkind = 'sim.biome'\ndescriptor = 'biome.toml'\n"}});
        source.m_descriptors.push_back(
            {root / "biome.toml", "schema_version = 1\n[definition]\ntemperature.base_k = 293.0\n"
                                  "[definition.composition.band]\nbed_period_m = 4.0\n"});
        config::Diagnostics index_diagnostics;
        auto index = asset::PackSet::index(source.m_packs, kContentAssetKinds, index_diagnostics);
        PPR_TEST_ASSERT(index.has_value());
        view.m_source = &source;
        view.m_index = &*index;
        view.m_revision = 1u;
        view.m_busy = true;
        view.m_dirty = true;

        // "Save documents" is a File-menu item. The menu bar pushes its own ID
        // scope, so the File menu's identity is hashed against "##MenuBar".
        const ImGuiID file_menu = ImHashStr("File", 0, ImHashStr("##MenuBar", 0, window->ID));

        const auto open_file_menu = [&]() {
            static_cast<void>(clickItem(window, file_menu, ImGuiNavLayer_Menu));
            PPR_TEST_ASSERT(not context->OpenPopupStack.empty());
            return context->OpenPopupStack.back().Window;
        };

        const auto save_documents = [&](ImGuiWindow *const popup) {
            // Only the text after "###" hashes; the icon and the visible label
            // are ui::icons::kSaveDocumentsLabel's decoration.
            return clickItem(popup, popup->GetID("###Save documents"), ImGuiNavLayer_Main);
        };

        PPR_TEST_ASSERT(not emitted(save_documents(open_file_menu()), EWorkshopIntent::save));
        view.m_busy = false;
        auto *const popup = context->OpenPopupStack.empty() ? open_file_menu() : context->OpenPopupStack.back().Window;
        PPR_TEST_ASSERT(emitted(save_documents(popup), EWorkshopIntent::save));

        const config::Diagnostic diagnostic{
            (root / "biome.toml").string() + ":5", "definition.composition.band.bed_period_m", {},
            "Expected a positive period", std::make_error_code(std::errc::result_out_of_range)};
        view.m_property_diagnostics = std::span{&diagnostic, 1u};
        view.m_diagnostics_revision = 1u;
        state.m_selected_key = "widget:hearth";
        state.m_focus_property = diagnostic.m_property;
        std::snprintf(state.m_property_search.data(), state.m_property_search.size(), "%s", "bed_period_m");
        static_cast<void>(frame(ImVec2{-1.0f, -1.0f}, false));
        PPR_TEST_ASSERT(not state.m_property_tree.empty() and state.m_focus_property.empty());
        PPR_TEST_ASSERT(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        ImGui::ClosePopupToLevel(0, false);
        std::snprintf(state.m_property_search.data(), state.m_property_search.size(), "%s", "temperature.base_k");
        static_cast<void>(frame(ImVec2{-1.0f, -1.0f}, false));
        // The live panel draws its property inspector inside the "Properties"
        // child of the workspace window. ImGui mangles child names as
        // "<parent>/Properties_%08X", so match the workspace prefix and the leaf.
        ImGuiWindow *properties_window{};
        for (auto *candidate: context->Windows) {
            const std::string_view name{candidate->Name};
            if (name.starts_with(window->Name) and name.find("/Properties_") != std::string_view::npos) {
                properties_window = candidate;
                break;
            }
        }

        PPR_TEST_ASSERT(properties_window != nullptr);
        // That child owns the panel's only two-column table, "Property tree";
        // find the drawn table through its owner rather than reconstructing the
        // child's internal ID stack.
        const ImGuiTable *table{};
        for (int table_index = 0; table_index < context->Tables.GetBufSize(); ++table_index) {
            const auto *const candidate = context->Tables.GetByIndex(table_index);
            if (candidate->OuterWindow == properties_window and candidate->ColumnsCount == 2) {
                table = candidate;
                break;
            }
        }

        PPR_TEST_ASSERT(table != nullptr);
        const float left = table->Columns[1u].WorkMinX;
        const float slider_width = table->Columns[1u].WorkMaxX - left - 38.0f;
        const float slider_y = table->RowPosY1 + ImGui::GetFrameHeight() * 0.5f;
        PPR_TEST_ASSERT(
            not emitted(frame(ImVec2{left + slider_width * 0.3f, slider_y}, true), EWorkshopIntent::edit_definition));
        PPR_TEST_ASSERT(
            not emitted(frame(ImVec2{left + slider_width * 0.7f, slider_y}, true), EWorkshopIntent::edit_definition));
        PPR_TEST_ASSERT(state.m_numeric_edit_key == "definition.temperature.base_k");
        const auto dragged = state.m_numeric_edit.m_floating;
        const auto released = frame(ImVec2{left + slider_width * 0.7f, slider_y}, false);
        const auto edit = std::ranges::find(released, EWorkshopIntent::edit_definition, &WorkshopIntent::m_kind);
        PPR_TEST_ASSERT(
            edit != released.end() and
            edit->m_property == "definition.temperature.base_k" and
            edit->m_value.m_floating == dragged);
        PPR_TEST_ASSERT(state.m_numeric_edit_key.empty());
    };

    struct OutputDirectory {
        std::filesystem::path m_parent{hal::process::currentExecutablePath().parent_path()};
        std::filesystem::path m_root;
        OutputDirectory() {
            static std::atomic<u32> ordinal{};
            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto candidate = m_parent / ("worldgen-preview-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(candidate)) {
                    m_root = candidate;
                    break;
                }
            }

            PPR_TEST_ASSERT(not m_root.empty());
        }

        ~OutputDirectory() {
            // Best-effort fixture cleanup only; never remove anything outside
            // the explicitly owned sibling directory created above.
            if (not m_root.empty() and m_root.parent_path() == m_parent) {
                std::error_code error;
                std::filesystem::remove_all(m_root, error);
            }
        }
    };

    std::string readBytes(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        PPR_TEST_ASSERT(file.good());
        return {std::istreambuf_iterator<char>{file}, {}};
    }

    std::shared_ptr<const ContentCatalog> selectFixture(
        const ContentSourceSet &source, config::Diagnostics &diagnostics,
        const sim::definition::GenerationCompileParameters parameters = kWorldCompileParameters) {
        auto definitions = loadContentCatalog(source, diagnostics);
        PPR_TEST_ASSERT(definitions.has_value());

        auto owner = std::make_shared<const ContentDescriptorCatalog>(std::move(*definitions));
        auto world = resolveContentWorld(*owner, "ppr_frontier:frontier", diagnostics);
        PPR_TEST_ASSERT(world.has_value());

        auto selected = selectWorldCatalog(std::move(owner), *world, parameters, diagnostics);
        PPR_TEST_ASSERT(selected.has_value());

        return std::make_shared<const ContentCatalog>(std::move(*selected));
    }

    void prepareFixturePresentation(WorldgenPreviewResult &preview, config::Diagnostics &diagnostics) {
        const asset::RequestGeneration generation{preview.m_spec.m_revision + 1009u};
        auto request = ContentPresentationRequest::start(
            preview.m_catalog, generation, context::background(), diagnostics);
        PPR_TEST_ASSERT(request.has_value());

        auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(result.has_value());

        PPR_TEST_ASSERT(not preview.attachPresentation(*result, generation));
        const auto previous = preview.m_presentation;
        PPR_TEST_ASSERT(preview.m_presentation_generation == generation);
        PPR_TEST_ASSERT(*generation != preview.m_spec.m_revision);

        PPR_TEST_ASSERT(preview.attachPresentation(*result, {}) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(preview.m_presentation == previous and preview.m_presentation_generation == generation);

        PPR_TEST_ASSERT(
            preview.attachPresentation(*result, asset::RequestGeneration{*generation + 1u}) ==
            std::errc::invalid_argument);
        PPR_TEST_ASSERT(preview.m_presentation == previous and preview.m_presentation_generation == generation);

        const auto catalog = preview.m_catalog;
        preview.m_catalog = std::make_shared<const ContentCatalog>(*catalog);
        PPR_TEST_ASSERT(preview.attachPresentation(*result, generation) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(preview.m_presentation == previous and preview.m_presentation_generation == generation);
        preview.m_catalog = catalog;
    }

    void verifySample(const WorldgenPreviewResult &sample, const sim::GlobalCellPos origin) {
        PPR_TEST_ASSERT(
            sample.m_visible ==
            (sim::GenerationCellBounds{
                origin.m_x, origin.m_y, origin.m_x + kWorldgenPreviewEdge, origin.m_y + kWorldgenPreviewEdge}));
        PPR_TEST_ASSERT(sample.m_grid.residentChunkCount() <= kWorldgenPreviewMaxResidentChunks);
        PPR_TEST_ASSERT(sample.m_grid.residentChunkCount() > 0u);
        PPR_TEST_ASSERT(sample.m_checksum != 0u);

        u64 cells{};
        u64 solid{};
        for (const auto count: sample.m_material_counts) {
            cells += count.m_cells;
            solid += count.m_material != 0u ? count.m_cells : 0u;
        }

        PPR_TEST_ASSERT(cells == 256u * 256u and solid == sample.m_solid_cells);
        PPR_TEST_ASSERT(sample.m_geometry.m_instances.empty());
        PPR_TEST_ASSERT(not sample.m_omissions.empty());
        PPR_TEST_ASSERT(sample.m_known_scratch_peak_bytes > 0u and sample.m_known_retained_payload_bytes > 0u);

        // The eight-metre formation uses this preview's actual cell size. Its
        // complete roots and four-cell support/validation ring must be resident.
        const auto formation_length = static_cast<u32>(std::ceil(8.0 / sample.m_plan.recipe().m_cell_size_m));
        const u32 reach = formation_length + 4u;
        const auto &domain = sample.m_formation_domain;
        const auto &support = sample.m_support;
        PPR_TEST_ASSERT(domain.m_left <= (origin.m_x > formation_length ? origin.m_x - formation_length : 0u));
        PPR_TEST_ASSERT(domain.m_bottom <= (origin.m_y > formation_length ? origin.m_y - formation_length : 0u));
        PPR_TEST_ASSERT(domain.m_right >= std::min(sim::kWorldEdge, origin.m_x + 256u + formation_length));
        PPR_TEST_ASSERT(domain.m_top >= std::min(sim::kWorldEdge, origin.m_y + 256u + formation_length));
        PPR_TEST_ASSERT(support.m_left <= (domain.m_left > reach ? domain.m_left - reach : 0u));
        PPR_TEST_ASSERT(support.m_bottom <= (domain.m_bottom > reach ? domain.m_bottom - reach : 0u));
        PPR_TEST_ASSERT(support.m_right >= std::min(sim::kWorldEdge, domain.m_right + reach));
        PPR_TEST_ASSERT(support.m_top >= std::min(sim::kWorldEdge, domain.m_top + reach));

        for (u32 y = support.m_bottom; y < support.m_top; y += sim::kChunkEdge) {
            for (u32 x = support.m_left; x < support.m_right; x += sim::kChunkEdge) {
                PPR_TEST_ASSERT(sample.m_grid.residentCells(sim::chunkOf({x, y})).has_value());
            }
        }
    }

    void verifyImages(const WorldgenPreviewResult &sample, const std::filesystem::path &directory) {
        constexpr std::string_view header{"P6\n256 256\n255\n"};
        constexpr std::size_t image_bytes = 256u * 256u * 3u;
        const auto materials = readBytes(directory / "material.ppm");
        const auto biomes = readBytes(directory / "biome.ppm");
        const auto temperatures = readBytes(directory / "temperature.ppm");
        for (const std::string_view bytes:
            {std::string_view{materials}, std::string_view{biomes}, std::string_view{temperatures}}) {
            PPR_TEST_ASSERT(bytes.starts_with(header) and bytes.size() == header.size() + image_bytes);
        }

        const u32 biome_rgba = sample.m_catalog->biomes()[sample.m_biome_index].m_preview_color_rgba;
        bool unequal_rows{};

        for (u32 row = 0u; row < 256u; ++row) {
            const u32 y = sample.m_visible.m_top - 1u - row;
            for (u32 column = 0u; column < 256u; ++column) {
                const u32 x = sample.m_visible.m_left + column;
                const auto cell = sample.m_grid.getCell({x, y});
                PPR_TEST_ASSERT(cell.has_value());
                const auto *const material = sample.m_catalog->findMaterial(cell->m_element);
                const u32 rgba = material ? material->m_color_rgba : 0u;
                const std::size_t pixel = header.size() + (row * 256u + column) * 3u;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const u32 shift = 24u - channel * 8u;
                    PPR_TEST_ASSERT(static_cast<u8>(materials[pixel + channel]) == ((rgba >> shift) & 255u));
                    PPR_TEST_ASSERT(static_cast<u8>(biomes[pixel + channel]) == ((biome_rgba >> shift) & 255u));
                }
                unequal_rows = unequal_rows or
                               materials[pixel] != materials[header.size() + ((255u - row) * 256u + column) * 3u];
            }
        }

        // This authored subsurface sample contains an asymmetric raster, so the
        // per-cell comparison above also detects a vertical image inversion.
        PPR_TEST_ASSERT(unequal_rows);
    }

    PPR_UNIT_TEST (authored_layouts_preserve_required_biomes_at_selected_sizes) {
        const auto root = hal::process::currentExecutablePath().parent_path() / "worldgen/default";
        config::Diagnostics diagnostics;
        auto loaded = loadContentCatalog(root, diagnostics);
        PPR_TEST_ASSERT(loaded.has_value());
        const auto definitions = std::make_shared<const ContentDescriptorCatalog>(std::move(*loaded));

        for (const std::string_view profile: {"ppr_frontier:frontier", "ppr_frontier:hollows"}) {
            const auto world_id = resolveContentWorld(*definitions, profile, diagnostics);
            PPR_TEST_ASSERT(world_id.has_value());

            for (const u32 edge: {4096u, 2048u, 1024u}) {
                const auto catalog = selectWorldCatalog(
                    definitions, *world_id, worldCompileParameters(edge), diagnostics);
                PPR_TEST_ASSERT(catalog.has_value());

                for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                    const auto plan = compileWorldPlan(*catalog, profile, seed);
                    PPR_TEST_ASSERT(plan.has_value());

                    const auto layout = sim::compileBiomeLayout(*plan);
                    if (not layout) {
                        std::println(
                            "LAYOUT-COVERAGE profile={} edge={} seed={} error={}", profile, edge, seed,
                            layout.error().message());
                    }
                    PPR_TEST_ASSERT(layout.has_value());

                    const auto repeated = sim::compileBiomeLayout(*plan);
                    PPR_TEST_ASSERT(repeated and std::ranges::equal(layout->sites(), repeated->sites()));

                    const auto &recipe = plan->recipe();
                    PPR_TEST_ASSERT(
                        layout->sites().size() >= recipe.m_min_sites and layout->sites().size() <= recipe.m_max_sites);
                    const auto start = layout->sites().front();
                    PPR_TEST_ASSERT(start.m_biome == recipe.m_start_biome);
                    Array<u32> counts(recipe.m_biomes.size(), 0u);
                    for (const auto &site: layout->sites()) {
                        PPR_TEST_ASSERT(
                            site.m_x_m >= 0.0 and
                            site.m_y_m >= 0.0 and
                            site.m_x_m < edge * recipe.m_cell_size_m and
                            site.m_y_m < edge * recipe.m_cell_size_m);
                        PPR_TEST_ASSERT(site.m_biome < counts.size());
                        ++counts[site.m_biome];

                        const auto constraint = std::ranges::find(
                            recipe.m_constraints, site.m_biome, &sim::GenerationBiomeConstraint::m_biome);
                        PPR_TEST_ASSERT(constraint != recipe.m_constraints.end());
                        const double depth = sim::generationDepth(*plan, site.m_x_m, site.m_y_m);
                        const double distance = std::hypot(site.m_x_m - start.m_x_m, site.m_y_m - start.m_y_m);
                        PPR_TEST_ASSERT(depth >= constraint->m_depth_min and depth <= constraint->m_depth_max);
                        PPR_TEST_ASSERT(
                            distance >= constraint->m_distance_min_m and distance <= constraint->m_distance_max_m);
                    }

                    for (const auto &constraint: recipe.m_constraints) {
                        PPR_TEST_ASSERT(counts[constraint.m_biome] >= constraint.m_required_count);
                    }
                }
            }
        }
    };

    PPR_UNIT_TEST (bounded_samples_replay_and_export_the_exact_visible_draft) {
        const auto root = hal::process::currentExecutablePath().parent_path() / "worldgen/default";
        const std::filesystem::path roots[] = {root};
        config::Diagnostics diagnostics;
        auto loaded = loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(loaded.has_value());
        auto source = loaded->source();
        const auto biome_id = loaded->packOwner()->resolve(
            "ppr_frontier", "biome_hearth", sim::asset::kBiomeKind, diagnostics);
        PPR_TEST_ASSERT(biome_id.has_value());
        auto biome = std::ranges::find(
            source.m_descriptors, (*biome_id)->m_descriptor, &asset::DescriptorSource::m_path);
        PPR_TEST_ASSERT(biome != source.m_descriptors.end());
        const auto patch = config::patchScalar(
            biome->m_bytes, "definition.formation.length_m", config::FileValue::floating(8.0), biome->m_path.string(),
            diagnostics);
        PPR_TEST_ASSERT(patch.has_value());
        auto edited = config::applySourcePatch(biome->m_bytes, *patch);
        PPR_TEST_ASSERT(edited.has_value());
        biome->m_bytes = std::move(*edited);

        // Authored matter-fill heights are thirds of a metre, so the preview must
        // compile against the fine-cell metric.
        const sim::definition::GenerationCompileParameters parameters{1.0 / 3.0, 2.0, 0.3, 0.7};
        const auto catalog = selectFixture(source, diagnostics, parameters);

        const WorldgenPreviewSpec spec{"ppr_frontier:frontier", "hearth", 0xfedcba9876543210ull, 73u, {1879u, 2993u}};
        auto sample = prepareWorldgenPreview(catalog, spec);
        PPR_TEST_ASSERT(sample.has_value());
        PPR_TEST_ASSERT(sample->m_plan.recipe().m_biomes[sample->m_biome_index].m_formation_length_m == 8.0);
        verifySample(*sample, spec.m_origin);
        {
            const auto replay = prepareWorldgenPreview(catalog, spec);
            PPR_TEST_ASSERT(replay.has_value() and replay->m_checksum == sample->m_checksum);
            PPR_TEST_ASSERT(
                replay->m_support == sample->m_support and replay->m_formation_domain == sample->m_formation_domain);
            for (u32 y = sample->m_visible.m_bottom; y < sample->m_visible.m_top; ++y) {
                for (u32 x = sample->m_visible.m_left; x < sample->m_visible.m_right; ++x) {
                    PPR_TEST_ASSERT(sample->m_grid.getCell({x, y}) == replay->m_grid.getCell({x, y}));
                }
            }
        }

        for (const sim::GlobalCellPos origin: {sim::GlobalCellPos{0u, 0u}, sim::GlobalCellPos{3840u, 3840u}}) {
            auto boundary_spec = spec;
            boundary_spec.m_origin = origin;
            const auto boundary = prepareWorldgenPreview(catalog, boundary_spec);
            PPR_TEST_ASSERT(boundary.has_value());
            verifySample(*boundary, origin);
        }
        auto invalid_spec = spec;
        invalid_spec.m_origin.m_x = 3841u;
        const auto out_of_bounds = prepareWorldgenPreview(catalog, invalid_spec);
        PPR_TEST_ASSERT(not out_of_bounds and out_of_bounds.error() == std::errc::result_out_of_range);
        invalid_spec = spec;
        invalid_spec.m_biome = "missing-biome";
        const auto missing_biome = prepareWorldgenPreview(catalog, invalid_spec);
        PPR_TEST_ASSERT(not missing_biome and missing_biome.error() == std::errc::invalid_argument);
        invalid_spec = spec;
        invalid_spec.m_profile = "missing-profile";
        const auto missing_profile = prepareWorldgenPreview(catalog, invalid_spec);
        PPR_TEST_ASSERT(not missing_profile and missing_profile.error() == std::errc::invalid_argument);
        const auto [context, cancel] = context::withCancel(context::background());
        cancel();
        const auto cancelled = prepareWorldgenPreview(catalog, spec, context);
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);

        OutputDirectory output;
        const auto successful = output.m_root / "complete";
        const WorldgenPreviewDisplay display{670.5, 1040.0, 1.75, 0u};
        PPR_TEST_ASSERT(exportWorldgenPreview(*sample, source, display, successful) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not std::filesystem::exists(successful));
        PPR_TEST_ASSERT(sample->m_presentation_generation == asset::RequestGeneration{});

        prepareFixturePresentation(*sample, diagnostics);

        const auto generation = sample->m_presentation_generation;
        sample->m_presentation_generation = asset::RequestGeneration{*generation + 1u};
        PPR_TEST_ASSERT(exportWorldgenPreview(*sample, source, display, successful) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not std::filesystem::exists(successful));
        sample->m_presentation_generation = generation;

        const auto selected_owner = sample->m_catalog;
        sample->m_catalog = std::make_shared<const ContentCatalog>(*selected_owner);
        PPR_TEST_ASSERT(exportWorldgenPreview(*sample, source, display, successful) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not std::filesystem::exists(successful));
        sample->m_catalog = selected_owner;

        PPR_TEST_ASSERT(not exportWorldgenPreview(*sample, source, display, successful));
        verifyImages(*sample, successful);
        const auto exported_root = successful / "source/packs/pack-ppr_frontier";
        PPR_TEST_ASSERT(readBytes(exported_root / "pack.toml") == source.m_packs.front().m_manifest.m_bytes);
        for (const auto &descriptor: source.m_descriptors) {
            PPR_TEST_ASSERT(
                readBytes(exported_root / descriptor.m_path.lexically_relative(source.m_packs.front().m_root)) ==
                descriptor.m_bytes);
        }

        PPR_TEST_ASSERT(
            readBytes(successful / "source/pack-set.toml").find("packs/pack-ppr_frontier") != std::string::npos);
        const auto report = readBytes(successful / "preview.json");
        PPR_TEST_ASSERT(report.find("\"semantics\":\"Biome sample\"") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"revision\":73") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"temperature_min_k\":200") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"temperature_max_k\":1000") != std::string::npos);
        PPR_TEST_ASSERT(
            report.find(std::format("\"cell_checksum\":\"{:016x}\"", sample->m_checksum)) != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"export_version\":2") != std::string::npos);
        PPR_TEST_ASSERT(report.find(std::format("\"cell_size_m\":{}", parameters.m_cell_size_m)) != std::string::npos);
        PPR_TEST_ASSERT(
            report.find(std::format("\"cells_per_metre\":{}", 1.0 / parameters.m_cell_size_m)) != std::string::npos);
        PPR_TEST_ASSERT(
            report.find(std::format("\"reference_thickness_m\":{}", parameters.m_reference_thickness_m)) !=
            std::string::npos);
        PPR_TEST_ASSERT(
            report.find(std::format("\"actor_half_width_m\":{}", parameters.m_actor_half_width_m)) !=
            std::string::npos);
        PPR_TEST_ASSERT(
            report.find(std::format("\"actor_half_height_m\":{}", parameters.m_actor_half_height_m)) !=
            std::string::npos);
        PPR_TEST_ASSERT(report.find("exported pack-set alone is not runnable") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"numeric_fingerprint\":") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"presentation_fingerprint\":") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"settings_fingerprint\":") != std::string::npos);
        for (const auto &entry: std::filesystem::recursive_directory_iterator(exported_root)) {
            PPR_TEST_ASSERT(entry.path().extension() != ".png");
        }

        auto different_source = source;
        different_source.m_descriptors.front().m_bytes += "\n# newer draft\n";
        const auto mismatched_output = output.m_root / "mismatched";
        PPR_TEST_ASSERT(
            exportWorldgenPreview(*sample, different_source, display, mismatched_output) ==
            std::errc::invalid_argument);
        PPR_TEST_ASSERT(not std::filesystem::exists(mismatched_output));

        const auto completion = readBytes(successful / "complete.json");
        PPR_TEST_ASSERT(completion.find("\"revision\":73") != std::string::npos);
        PPR_TEST_ASSERT(exportWorldgenPreview(*sample, source, display, successful) == std::errc::file_exists);
        PPR_TEST_ASSERT(readBytes(successful / "complete.json") == completion);
        const auto cancelled_output = output.m_root / "cancelled";
        PPR_TEST_ASSERT(
            exportWorldgenPreview(*sample, source, display, cancelled_output, context.get()) ==
            std::errc::operation_canceled);
        PPR_TEST_ASSERT(not std::filesystem::exists(cancelled_output / "complete.json"));
    };

    PPR_UNIT_TEST (whole_world_crops_preserve_production_formations_seams_and_matter) {
        struct Clock final : ITimerClock {
            mutable u32 m_calls{};
            TimePoint now() const noexcept override {
                ++m_calls;
                return time::now();
            }
        } clock;
        const auto root = hal::process::currentExecutablePath().parent_path() / "worldgen/default";
        const std::filesystem::path roots[] = {root};
        config::Diagnostics diagnostics;
        auto loaded = loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(loaded.has_value());
        auto source = loaded->source();
        // Exercise formation limits in every declared Biome descriptor, keeping
        // the full explicit palette and every other production terrain phase.
        for (const auto &entry: loaded->packOwner()->assets()) {
            if (entry.m_kind != sim::asset::kBiomeKind) {
                continue;
            }

            auto descriptor = std::ranges::find(
                source.m_descriptors, entry.m_descriptor, &asset::DescriptorSource::m_path);
            PPR_TEST_ASSERT(descriptor != source.m_descriptors.end());
            for (const auto &[path, value]:
                {std::pair{"definition.formation.probability", 0.2}, std::pair{"definition.formation.length_m", 8.0}}) {
                auto patch = config::patchScalar(
                    descriptor->m_bytes, path, config::FileValue::floating(value), descriptor->m_path.string(),
                    diagnostics);
                PPR_TEST_ASSERT(patch.has_value());
                auto changed = config::applySourcePatch(descriptor->m_bytes, *patch);
                PPR_TEST_ASSERT(changed.has_value());
                descriptor->m_bytes = std::move(*changed);
            }
        }

        const auto catalog = selectFixture(source, diagnostics);
        WorldgenPreviewSpec spec{
            "ppr_frontier:frontier", "ignored-in-world-mode", 42u, 9u, {127u, 127u},
            EWorldgenPreviewMode::whole_world_crop};
        const auto missing = prepareWorldgenPreview(catalog, spec);
        PPR_TEST_ASSERT(not missing and missing.error() == std::errc::invalid_argument);
        ParallelScheduler generation_pool{
            "Preview generation test", std::max(1u, std::min(8u, std::thread::hardware_concurrency())),
            4u * static_cast<std::size_t>(hal::page_granularity)};
        const auto prepared = prepareWorldgenPreviewWorld(*catalog, spec, clock, nullptr, &generation_pool);
        PPR_TEST_ASSERT(prepared.has_value() and clock.m_calls > 0u);
        const auto &world = (*prepared)->m_world;
        for (std::size_t phase = 0u; phase < world.m_report.m_phase_times.size(); ++phase) {
            std::println(
                "Workshop exact production phase {}: {:.3f}s", phase,
                time::seconds(world.m_report.m_phase_times[phase]));
        }

        std::println(
            "Workshop exact production checksums: terrain={:016x}, output={:016x}", world.m_report.m_terrain_checksum,
            world.m_report.m_output_checksum);
        std::println(
            "Workshop exact production known cache payload: {} bytes", (*prepared)->m_known_retained_payload_bytes);

        PPR_TEST_ASSERT(world.m_grid.residentChunkCount() == sim::kChunkCount);
        PPR_TEST_ASSERT(world.m_report.m_output_checksum != 0u);
        PPR_TEST_ASSERT((*prepared)->matches(*catalog, spec));
        auto raster = sim::compileGenerationRaster(world.m_plan, world.m_layout, world.m_geometry);
        PPR_TEST_ASSERT(raster.has_value());
        std::optional<sim::GlobalCellPos> formation;
        for (u32 cy = 0u; cy < sim::kChunksPerEdge and not formation; ++cy) {
            for (u32 cx = 0u; cx < sim::kChunksPerEdge and not formation; ++cx) {
                const auto final = world.m_grid.residentCells({cx, cy});
                PPR_TEST_ASSERT(final.has_value());
                const bool candidate = std::ranges::any_of(*final, [&](const sim::Cell cell) {
                    return cell.m_element != 0u and
                           std::ranges::any_of(world.m_plan.recipe().m_biomes, [&](const auto &biome) {
                               return cell.m_element == biome.m_formation_material or
                                      cell.m_element == biome.m_rind_material;
                           });
                });
                if (not candidate) {
                    continue;
                }

                const auto before = raster->chunk({cx, cy});
                PPR_TEST_ASSERT(before.has_value());
                for (u32 index = 0u; index < sim::kCellsPerChunk; ++index) {
                    if ((*before)[index] != (*final)[index]) {
                        formation = sim::GlobalCellPos{
                            cx * sim::kChunkEdge + index % sim::kChunkEdge,
                            cy * sim::kChunkEdge + index / sim::kChunkEdge};
                        break;
                    }
                }
            }
        }

        PPR_TEST_ASSERT(formation.has_value());
        const auto cropOrigin = [](const sim::GlobalCellPos center) {
            return sim::GlobalCellPos{
                std::min(center.m_x > 127u ? center.m_x - 127u : 0u, sim::kWorldEdge - kWorldgenPreviewEdge),
                std::min(center.m_y > 127u ? center.m_y - 127u : 0u, sim::kWorldEdge - kWorldgenPreviewEdge)};
        };

        PPR_TEST_ASSERT(not world.m_matter.m_regions.empty());
        const auto &matter_region = world.m_matter.m_regions.front();
        PPR_TEST_ASSERT(not matter_region.m_spans.empty());
        const auto matter_span = matter_region.m_spans.front();
        const sim::GlobalCellPos matter_cell{matter_span.m_begin_x, matter_span.m_y};
        const auto matter_origin = cropOrigin(matter_cell);
        const auto matter_batch = std::ranges::find(
            (*prepared)->m_particles.m_chunks, sim::chunkOf(matter_cell), &sim::MatterParticleBatch::m_position);
        PPR_TEST_ASSERT(matter_batch != (*prepared)->m_particles.m_chunks.end());
        PPR_TEST_ASSERT(not matter_batch->records().empty());
        const auto matter_particle = std::ranges::find_if(matter_batch->records(), [&](const auto &particle) {
            return particle.m_x_m == (matter_cell.m_x + 0.5) * world.m_plan.recipe().m_cell_size_m and
                   particle.m_y_m == (matter_cell.m_y + 0.5) * world.m_plan.recipe().m_cell_size_m;
        });
        PPR_TEST_ASSERT(matter_particle != matter_batch->records().end());
        for (const auto origin:
            {spec.m_origin, sim::GlobalCellPos{3840u, 3840u}, cropOrigin(*formation),
                cropOrigin({world.m_plan.recipe().m_start_x_fine, world.m_plan.recipe().m_start_y_fine}),
                matter_origin}) {
            spec.m_origin = origin;
            ++spec.m_revision;
            const u32 clock_calls = clock.m_calls;
            const auto crop = prepareWorldgenPreview(catalog, spec, nullptr, *prepared);
            PPR_TEST_ASSERT(crop.has_value() and clock.m_calls == clock_calls);
            PPR_TEST_ASSERT(crop->m_spec == spec and crop->m_exact_world == *prepared);
            PPR_TEST_ASSERT(crop->m_grid.residentChunkCount() <= 16u);
            PPR_TEST_ASSERT(std::addressof(crop->geometry()) == std::addressof(world.m_geometry));
            PPR_TEST_ASSERT(not crop->m_layout.sampleBiome());
            u64 cell_count{};
            for (const auto count: crop->m_material_counts) {
                cell_count += count.m_cells;
            }

            PPR_TEST_ASSERT(cell_count == 256u * 256u);
            PPR_TEST_ASSERT(crop->m_known_world_cache_payload_bytes >= world.m_report.m_retained_known_payload_bytes);
            for (u32 y = crop->m_visible.m_bottom; y < crop->m_visible.m_top; ++y) {
                for (u32 x = crop->m_visible.m_left; x < crop->m_visible.m_right; ++x) {
                    PPR_TEST_ASSERT(crop->m_grid.getCell({x, y}) == world.m_grid.getCell({x, y}));
                    const double xm = (x + 0.5) * world.m_plan.recipe().m_cell_size_m,
                                 ym = (y + 0.5) * world.m_plan.recipe().m_cell_size_m;
                    PPR_TEST_ASSERT(
                        sim::generationBiomeAt(crop->m_plan, crop->m_layout, xm, ym) ==
                        sim::generationBiomeAt(world.m_plan, world.m_layout, xm, ym));
                }
            }

            for (const auto &batch: crop->m_particles.m_chunks) {
                const auto expected = std::ranges::find(
                    (*prepared)->m_particles.m_chunks, batch.m_position, &sim::MatterParticleBatch::m_position);
                PPR_TEST_ASSERT(expected != (*prepared)->m_particles.m_chunks.end());
                PPR_TEST_ASSERT(expected->m_particles == batch.m_particles);
            }

            for (const auto &region: crop->m_matter.m_regions) {
                const auto expected = std::ranges::find(
                    world.m_matter.m_regions, region.m_identity, &sim::InitialMatterRegion::m_identity);
                PPR_TEST_ASSERT(expected != world.m_matter.m_regions.end());
                PPR_TEST_ASSERT(region.m_material == expected->m_material and region.m_mass_kg > 0.0);
                for (const auto span: region.m_spans) {
                    PPR_TEST_ASSERT(
                        span.m_y >= crop->m_visible.m_bottom and
                        span.m_y < crop->m_visible.m_top and
                        span.m_begin_x >= crop->m_visible.m_left and
                        span.m_end_x <= crop->m_visible.m_right);
                }
            }

            if (origin == matter_origin) {
                PPR_TEST_ASSERT(not crop->m_matter.m_regions.empty() and not crop->m_particles.m_chunks.empty());
                const auto clipped_region = std::ranges::find(
                    crop->m_matter.m_regions, matter_region.m_identity, &sim::InitialMatterRegion::m_identity);
                PPR_TEST_ASSERT(clipped_region != crop->m_matter.m_regions.end());
                PPR_TEST_ASSERT(std::ranges::any_of(clipped_region->m_spans, [&](const auto span) {
                    return span.m_y == matter_cell.m_y and
                           span.m_begin_x <= matter_cell.m_x and
                           matter_cell.m_x < span.m_end_x;
                }));
                const auto clipped_batch = std::ranges::find(
                    crop->m_particles.m_chunks, matter_batch->m_position, &sim::MatterParticleBatch::m_position);
                PPR_TEST_ASSERT(clipped_batch != crop->m_particles.m_chunks.end());
                const auto clipped_particle = std::ranges::find(
                    clipped_batch->records(), matter_particle->m_id, &sim::MatterParticle::m_id);
                PPR_TEST_ASSERT(clipped_particle != clipped_batch->records().end());
                PPR_TEST_ASSERT(*clipped_particle == *matter_particle);
            }
        }
        auto mismatch = spec;
        ++mismatch.m_seed;
        const auto rejected = prepareWorldgenPreview(catalog, mismatch, nullptr, *prepared);
        PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);
        const auto [context, cancel] = context::withCancel(context::background());
        cancel();
        const u32 clock_calls = clock.m_calls;
        const auto cancelled_world = prepareWorldgenPreviewWorld(*catalog, spec, clock, context);
        PPR_TEST_ASSERT(not cancelled_world and cancelled_world.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(clock.m_calls == clock_calls);
        const auto cancelled_crop = prepareWorldgenPreview(catalog, spec, context, *prepared);
        PPR_TEST_ASSERT(not cancelled_crop and cancelled_crop.error() == std::errc::operation_canceled);

        spec.m_origin = cropOrigin(*formation);
        auto crop = prepareWorldgenPreview(catalog, spec, nullptr, *prepared);
        PPR_TEST_ASSERT(crop.has_value());
        prepareFixturePresentation(*crop, diagnostics);

        OutputDirectory output;
        const auto directory = output.m_root / "exact";
        PPR_TEST_ASSERT(not exportWorldgenPreview(*crop, source, {}, directory));
        const auto report = readBytes(directory / "preview.json");
        PPR_TEST_ASSERT(report.find("\"semantics\":\"Exact whole-world crop\"") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"whole_world_preparation\":") != std::string::npos);
        constexpr std::string_view header{"P6\n256 256\n255\n"};
        const auto biomes = readBytes(directory / "biome.ppm");
        const auto matter = readBytes(directory / "initial-matter.ppm");
        PPR_TEST_ASSERT(biomes.starts_with(header) and biomes.size() == header.size() + 256u * 256u * 3u);
        PPR_TEST_ASSERT(matter.starts_with(header) and matter.size() == biomes.size());
        for (u32 row = 0u; row < 256u; ++row) {
            for (u32 column = 0u; column < 256u; ++column) {
                const u32 x = crop->m_visible.m_left + column, y = crop->m_visible.m_top - 1u - row;
                const u32 biome = sim::generationBiomeAt(
                    world.m_plan, world.m_layout, (x + 0.5) * world.m_plan.recipe().m_cell_size_m,
                    (y + 0.5) * world.m_plan.recipe().m_cell_size_m);
                const u32 rgba = catalog->biomes()[biome].m_preview_color_rgba;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT(
                        static_cast<u8>(biomes[header.size() + (row * 256u + column) * 3u + channel]) ==
                        ((rgba >> (24u - channel * 8u)) & 255u));
                }
            }
        }

        PPR_TEST_ASSERT(std::filesystem::exists(directory / "complete.json"));
    };
}

namespace pP::game::tests {
    const UnitTest worldgen_preview = UnitTest::Named("worldgen_preview") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::worldgen_preview::lighting_showcase_has_catalog_backed_cells_and_canonical_actor_clearance,
                detail::worldgen_preview::bounded_samples_replay_and_export_the_exact_visible_draft,
                detail::worldgen_preview::workshop_widgets_block_invalid_actions_and_focus_nested_property_errors,
                detail::worldgen_preview::authored_layouts_preserve_required_biomes_at_selected_sizes,
                detail::worldgen_preview::whole_world_crops_preserve_production_formations_seams_and_matter});
    };

    const UnitTest &gameWorldgenPreviewTests() noexcept {
        return worldgen_preview;
    }
}
