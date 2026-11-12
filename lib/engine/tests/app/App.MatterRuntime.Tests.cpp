module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module engine.tests.app;

import engine.core;
import engine.app;
import engine.sim;
import engine.physics;
import engine.matter;
import engine.shader;
import engine.rhi;
import game.colony.matter;
import game.colony.driver;
import engine.asset;
import engine.image;
import game.colony.content;
import game.colony.worldgen;
import std;

#include "Colony.AuthoredFixture.Tests.h"

namespace pP::tests::detail::MatterAdapter {
    // ------------------------------------------------------------------
    // host-native GPU fixture backend
    // ------------------------------------------------------------------

#ifdef _WIN32
    constexpr auto kFixtureBackend = rhi::DeviceType::D3D12;
#else
    constexpr auto kFixtureBackend = rhi::DeviceType::Vulkan;
#endif

    // ------------------------------------------------------------------
    // owned request execution for explicitly inline fixtures
    // ------------------------------------------------------------------

    colony::MatterCompletion completeInline(colony::MatterRuntime &runtime, Expected<colony::MatterRequest> request) {
        PPR_TEST_ASSERT(request.has_value());

        auto completion = runtime.advance(*request);
        PPR_TEST_ASSERT(completion and completion->has_value());
        PPR_TEST_ASSERT((**completion).m_execution.m_ran);
        PPR_TEST_ASSERT(not (**completion).m_execution.m_transport_error);

        return std::move(**completion);
    }

    rhi::ComPtr<rhi::ICommandQueue> acquireGraphicsQueue(rhi::IDevice &device) {
        rhi::ComPtr<rhi::ICommandQueue> queue;
        PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, queue.writeRef())));
        PPR_TEST_ASSERT(queue != nullptr);
        return queue;
    }

    Array<GridParticleMetadataRow> readMetadata(
        rhi::IDevice &device, rhi::ICommandQueue &queue, const GridParticleMetadataResource &metadata) {
        PPR_TEST_ASSERT(not make_error_code(queue.waitOnHost()));
        Array<GridParticleMetadataRow> rows(metadata.m_count);
        PPR_TEST_ASSERT(metadata.m_rows != nullptr);
        PPR_TEST_ASSERT(not make_error_code(
            device.readBuffer(metadata.m_rows, 0u, rows.size() * sizeof(GridParticleMetadataRow), rows.data())));
        return rows;
    }

    // ------------------------------------------------------------------
    // physical inventory and closed-world presentation
    // ------------------------------------------------------------------

    sim::MatterParticleSnapshot records() {
        Array<sim::MatterParticle> rows{
            {20u, 0.75, 0.75, 0.5, 750.0, 290.0, 1500.0, 2u, sim::EGenerationMatter::solid},
            {10u, 0.25, 0.75, 0.5, 500.0, 300.0, 1000.0, 1u, sim::EGenerationMatter::liquid},
            {30u, 1.25, 0.75, 0.5, 0.5, 310.0, 1.0, 3u, sim::EGenerationMatter::gas}};
        sim::MatterParticleSnapshot result{.m_next_id = 31u};
        result.m_chunks.push_back({{0u, 0u}, std::make_shared<const Array<sim::MatterParticle>>(std::move(rows))});
        return result;
    }

    PPR_UNIT_TEST (source_units_defaults_materials_and_unsimulated_gas_preserve_inventory) {
        std::fprintf(
            stderr,
            "[matter-layout] identity=%zu/%zu state=%zu/%zu readback=%zu/%zu runtime=%zu/%zu driver=%zu/%zu ledgerrow=%zu/%zu censuscell=%zu/%zu\n",
            sizeof(colony::MatterIdentity), alignof(colony::MatterIdentity), sizeof(colony::MatterState),
            alignof(colony::MatterState), sizeof(colony::MatterReadback), alignof(colony::MatterReadback),
            sizeof(colony::MatterRuntime), alignof(colony::MatterRuntime), sizeof(colony::ColonyDriver),
            alignof(colony::ColonyDriver), sizeof(colony::MatterPhysicalLedgerRow),
            alignof(colony::MatterPhysicalLedgerRow), sizeof(colony::MatterCensusCell),
            alignof(colony::MatterCensusCell));

        const auto seeded = records();
        const auto result = colony::makeMatterState(seeded, 0.5, 2.0, -9.8f, 71u);
        PPR_TEST_ASSERT(result.has_value());
        const auto &state = *result;
        PPR_TEST_ASSERT(state.m_particles.size() == 2u and state.m_gas.size() == 1u);
        PPR_TEST_ASSERT(state.m_records[0u].m_id == 10u and state.m_records[1u].m_id == 20u);
        PPR_TEST_ASSERT(state.m_particles[0u].m_mass == 1.0f and state.m_particles[0u].m_volume == 1.0f);
        PPR_TEST_ASSERT(state.m_particles[1u].m_mass == 1.5f and state.m_particles[1u].m_material == 2.0f);
        PPR_TEST_ASSERT(state.m_particles[0u].m_material == 0.0f);
        PPR_TEST_ASSERT(state.m_profile.m_grid_width == 4103u and state.m_profile.m_grid_height == 4103u);
        PPR_TEST_ASSERT(
            state.m_profile.m_gravity_strength ==
            static_cast<float>(static_cast<double>(std::abs(-9.8f)) / (4103u * 0.5)));
        PPR_TEST_ASSERT(state.m_profile.m_delta_time == 1.0f / 480.0f);
        PPR_TEST_ASSERT(state.m_identity.m_world == 71u and state.m_identity.m_publication == 0u);

        for (const auto &particle: state.m_particles) {
            PPR_TEST_ASSERT(particle.m_displacement_x == 0.0f and particle.m_displacement_y == 0.0f);
            PPR_TEST_ASSERT(
                particle.m_deformation_gradient.m_c0x == 1.0f and particle.m_deformation_gradient.m_c1y == 1.0f);
            PPR_TEST_ASSERT(
                particle.m_deformation_gradient.m_c0y == 0.0f and particle.m_deformation_gradient.m_c1x == 0.0f);
            PPR_TEST_ASSERT(
                particle.m_deformation_displacement.m_c0x == 0.0f and
                particle.m_deformation_displacement.m_c1y == 0.0f);
            PPR_TEST_ASSERT(particle.m_liquid_density == 1.0f and particle.m_log_jp == 1.0f);
        }

        const auto captured = colony::captureMovingMatter(state, state.m_particles);
        PPR_TEST_ASSERT(captured and captured->m_next_id == seeded.m_next_id);
        PPR_TEST_ASSERT(captured->m_chunks.size() == 1u and captured->m_chunks[0u].records().size() == 3u);
        PPR_TEST_ASSERT(captured->m_chunks[0u].records()[2u] == state.m_gas[0u]);
        PPR_TEST_ASSERT(seeded.m_chunks[0u].records()[0u].m_id == 20u);

        // Authoritative double volumes are independent of display radii/Cold24.
        const std::shared_ptr<const GridParticleMetadataResource> metadata =
            std::make_shared<GridParticleMetadataResource>();
        auto ledger = std::make_shared<colony::MatterPhysicalLedger>();
        ledger->m_rows = {{10u, 0.125, 1u}, {20u, 0.25, 1u}, {30u, 0.5, 3u}};
        ledger->m_metadata_owner = metadata;
        const GridParticlePresentationIdentity identity{{7u, 71u, 9u}, 3u, 1u, 4u};
        GridParticleCensus projection{};
        projection.m_metadata_owner = metadata;
        projection.m_identity = identity;
        projection.m_device_epoch = 6u;
        projection.m_submission = FenceCompletionValue{8u};
        projection.m_world_edge = 4096u;
        projection.m_rows = {{10u, 0u, 1u, 0u, 2u, 3u}, {20u, 0u, 1u, 0u, 2u, 3u}, {30u, 0u, 3u, 0u, 2u, 3u}};
        const auto aggregate = colony::aggregateMatterCensus(*ledger, projection, metadata, identity, 6u, 4096u);
        PPR_TEST_ASSERT(aggregate.has_value());
        PPR_TEST_ASSERT(aggregate->m_particle_count == 3u and aggregate->m_cells.size() == 2u);
        PPR_TEST_ASSERT(aggregate->m_total_volume_m3 == 0.875);
        PPR_TEST_ASSERT(aggregate->m_cells[0u].m_volume_m3 == 0.375 and aggregate->m_cells[0u].m_count == 2u);
        PPR_TEST_ASSERT(aggregate->m_cells[1u].m_volume_m3 == 0.5 and aggregate->m_cells[1u].m_material == 3u);

        const auto rejects = [&](const GridParticleCensus &rows, const GridParticlePresentationIdentity &cohort,
                                 const u64 epoch) {
            const auto denied = colony::aggregateMatterCensus(*ledger, rows, metadata, cohort, epoch, 4096u);
            PPR_TEST_ASSERT(not denied and denied.error() == std::errc::invalid_argument);
        };

        auto rejected = projection;
        rejected.m_rows[1u].m_id_low = 10u;
        rejects(rejected, identity, 6u);

        rejected = projection;
        rejected.m_rows.pop_back();
        rejects(rejected, identity, 6u);

        rejected = projection;
        rejected.m_rows[1u].m_material = 2u;
        rejects(rejected, identity, 6u);

        auto stale = identity;
        ++stale.m_publication;
        rejects(projection, stale, 6u);
        rejects(projection, identity, 7u);
        rejected = projection;
        rejected.m_submission = kFenceValueNone;
        rejects(rejected, identity, 6u);

        // Identical numeric identity/IDs/materials cannot substitute another
        // metadata owner's different authoritative physical volume.
        const std::shared_ptr<const GridParticleMetadataResource> next_metadata =
            std::make_shared<GridParticleMetadataResource>();
        auto next_ledger = std::make_shared<colony::MatterPhysicalLedger>(*ledger);
        next_ledger->m_metadata_owner = next_metadata;
        next_ledger->m_rows[0u].m_volume_m3 = 0.25;
        const auto wrong_ledger = colony::aggregateMatterCensus(
            *next_ledger, projection, metadata, identity, 6u, 4096u);
        PPR_TEST_ASSERT(not wrong_ledger and wrong_ledger.error() == std::errc::invalid_argument);

        const auto wrong_projection = colony::aggregateMatterCensus(
            *next_ledger, projection, next_metadata, identity, 6u, 4096u);
        PPR_TEST_ASSERT(not wrong_projection and wrong_projection.error() == std::errc::invalid_argument);

        auto next_projection = projection;
        next_projection.m_metadata_owner = next_metadata;
        const auto next = colony::aggregateMatterCensus(
            *next_ledger, next_projection, next_metadata, identity, 6u, 4096u);
        PPR_TEST_ASSERT(next.has_value() and next->m_total_volume_m3 == 1.0);

        // A later publication cannot mutate the completed owned census.
        ledger = std::move(next_ledger);
        PPR_TEST_ASSERT(aggregate->m_total_volume_m3 == 0.875 and aggregate->m_cells[0u].m_count == 2u);
    };

    PPR_UNIT_TEST (selected_map_edges_preserve_units_and_close_particle_bounds) {
        std::fprintf(
            stderr, "[map-runtime-layout] state=%zu/%zu driver_desc=%zu/%zu\n", sizeof(colony::MatterState),
            alignof(colony::MatterState), sizeof(colony::ColonyDriverDesc), alignof(colony::ColonyDriverDesc));

        for (const u32 edge: {1024u, 2048u, 4096u}) {
            constexpr double cell_metres = 1.0 / 3.0;
            auto state = colony::makeMatterState(records(), cell_metres, 1.0, colony::kMatterGravityY, 71u, edge);
            PPR_TEST_ASSERT(state.has_value());
            PPR_TEST_ASSERT(state->m_world_edge == edge);
            PPR_TEST_ASSERT(state->m_profile.m_grid_width == edge + 7u and state->m_profile.m_grid_height == edge + 7u);
            const double physical_gravity = static_cast<double>(state->m_profile.m_gravity_strength) *
                                            (edge + 7u) *
                                            cell_metres;
            PPR_TEST_ASSERT(
                std::abs(physical_gravity - std::abs(static_cast<double>(colony::kMatterGravityY))) < 0.00001);
            PPR_TEST_ASSERT(
                not colony::validateMatterProfile(state->m_profile, cell_metres, 1.0, colony::kMatterGravityY, edge));
            const u32 different_edge = edge == 1024u ? 2048u : 1024u;
            PPR_TEST_ASSERT(
                colony::validateMatterProfile(
                    state->m_profile, cell_metres, 1.0, colony::kMatterGravityY, different_edge) ==
                std::errc::invalid_argument);

            for (auto &particle: state->m_particles) {
                particle.m_position_x = static_cast<float>(edge) + colony::kMatterOrigin;
                particle.m_position_y = static_cast<float>(edge) + colony::kMatterOrigin;
            }

            const auto captured = colony::captureMovingMatter(*state, state->m_particles);
            PPR_TEST_ASSERT(captured.has_value());
            const sim::ChunkPos final_chunk{edge / sim::kChunkEdge - 1u, edge / sim::kChunkEdge - 1u};
            const auto last = std::ranges::find(captured->m_chunks, final_chunk, &sim::MatterParticleBatch::m_position);
            PPR_TEST_ASSERT(last != captured->m_chunks.end() and last->records().size() == 2u);
            PPR_TEST_ASSERT(last->records()[0u].m_x_m == edge * cell_metres);

            state->m_particles[0u].m_position_x += 0.125f;
            const auto outside = colony::captureMovingMatter(*state, state->m_particles);
            PPR_TEST_ASSERT(not outside and outside.error() == std::errc::result_out_of_range);

            if (edge != sim::kWorldEdge) {
                sim::ChunkGrid grid{edge};
                PPR_TEST_ASSERT(not grid.setCell({edge - 1u, edge - 1u}, sim::Cell{.m_element = 2u}));
                const std::array<u16, 1u> solids{2u};
                const auto masks = colony::makeMatterTerrain(grid, solids);
                PPR_TEST_ASSERT(masks and masks->size() == static_cast<std::size_t>(edge + 1u) * (edge + 1u));
                PPR_TEST_ASSERT(masks->back() == 15u);
                PPR_TEST_ASSERT((*masks)[20u * (edge + 1u) + 20u] == 0u);
            }
        }

        const auto invalid = colony::makeMatterState({}, 1.0, 1.0, colony::kMatterGravityY, 71u, 1536u);
        PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (moving_records_allow_overlap_detached_solids_and_exact_upper_endpoint) {
        const auto seeded = records();
        auto state = colony::makeMatterState(seeded, 0.5, 2.0, -9.8f, 71u);
        PPR_TEST_ASSERT(state.has_value());

        for (auto &particle: state->m_particles) {
            particle.m_position_x = 4099.0f;
            particle.m_position_y = 4099.0f;
        }

        const auto captured = colony::captureMovingMatter(*state, state->m_particles);
        PPR_TEST_ASSERT(captured.has_value());
        const auto last = std::ranges::find_if(
            captured->m_chunks, [](const auto &batch) { return batch.m_position == sim::ChunkPos{31u, 31u}; });
        PPR_TEST_ASSERT(last != captured->m_chunks.end() and last->records().size() == 2u);
        PPR_TEST_ASSERT(last->records()[0u].m_x_m == 2048.0 and last->records()[1u].m_y_m == 2048.0);
        PPR_TEST_ASSERT(last->records()[0u].m_mass_kg == 500.0 and last->records()[1u].m_material == 2u);

        const std::array<sim::GenerationMaterial, 3u> materials{
            sim::GenerationMaterial{1u, false, false, sim::EGenerationMatter::liquid, 1000.0},
            sim::GenerationMaterial{2u, true, true, sim::EGenerationMatter::solid, 1500.0},
            sim::GenerationMaterial{3u, false, false, sim::EGenerationMatter::gas, 1.0}};

        PPR_TEST_ASSERT(not sim::validateMovingMatterParticles(*captured, materials, 0.5, nullptr, 2.0));
        PPR_TEST_ASSERT(
            sim::validateMatterParticles(*captured, materials, 0.5, nullptr, 2.0) == std::errc::invalid_argument);

        sim::MatterParticleState current{};
        PPR_TEST_ASSERT(not current.restoreMoving(*captured, materials, 0.5, 2.0));
        PPR_TEST_ASSERT(current.count() == 3u and current.chunk({31u, 31u}).size() == 2u);
        PPR_TEST_ASSERT(seeded.m_chunks[0u].records()[1u].m_x_m == 0.25);
    };

    PPR_UNIT_TEST (moving_validation_preserves_static_cell_and_global_identity_guards) {
        const std::array<sim::GenerationMaterial, 3u> materials{
            sim::GenerationMaterial{1u, false, false, sim::EGenerationMatter::liquid, 1000.0},
            sim::GenerationMaterial{2u, false, false, sim::EGenerationMatter::solid, 1500.0},
            sim::GenerationMaterial{3u, false, false, sim::EGenerationMatter::gas, 1.0}};
        const auto original = records();
        Array<sim::MatterParticle> overlap{
            original.m_chunks[0u].records().begin(), original.m_chunks[0u].records().end()};
        overlap[1u].m_x_m = overlap[0u].m_x_m;
        overlap[1u].m_y_m = overlap[0u].m_y_m;

        const auto snapshot = [&](Array<sim::MatterParticle> rows) {
            sim::MatterParticleSnapshot result{.m_next_id = original.m_next_id};
            result.m_chunks.push_back({{0u, 0u}, std::make_shared<const Array<sim::MatterParticle>>(std::move(rows))});
            return result;
        };

        const auto valid = snapshot(overlap);
        PPR_TEST_ASSERT(not sim::validateMatterParticles(original, materials, 0.5, nullptr, 2.0));
        PPR_TEST_ASSERT(
            sim::validateMatterParticles(valid, materials, 0.5, nullptr, 2.0) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not sim::validateMovingMatterParticles(valid, materials, 0.5, nullptr, 2.0));

        sim::MatterParticleState current;
        PPR_TEST_ASSERT(not current.restoreMoving(valid, materials, 0.5, 2.0));
        const auto before = current.capture();

        const auto rejects = [&](const sim::MatterParticleSnapshot &invalid) {
            PPR_TEST_ASSERT(
                sim::validateMatterParticles(invalid, materials, 0.5, nullptr, 2.0) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(
                sim::validateMovingMatterParticles(invalid, materials, 0.5, nullptr, 2.0) ==
                std::errc::invalid_argument);
            PPR_TEST_ASSERT(current.restoreMoving(invalid, materials, 0.5, 2.0) == std::errc::invalid_argument);

            const auto after = current.capture();
            PPR_TEST_ASSERT(current.count() == 3u and after.m_next_id == before.m_next_id);
            PPR_TEST_ASSERT(after.m_chunks.size() == before.m_chunks.size());
            PPR_TEST_ASSERT(after.m_chunks[0u].m_particles == before.m_chunks[0u].m_particles);
        };

        auto rows = overlap;
        rows[1u].m_id = rows[0u].m_id;
        rejects(snapshot(rows));

        auto across = original;
        rows = {overlap[0u]};
        rows[0u].m_x_m = 64.25;
        across.m_chunks.push_back({{1u, 0u}, std::make_shared<const Array<sim::MatterParticle>>(std::move(rows))});
        rejects(across);

        rows = overlap;
        rows[0u].m_id = 0u;
        rejects(snapshot(rows));

        rows[0u].m_id = original.m_next_id;
        rejects(snapshot(rows));

        rows = overlap;
        rows[0u].m_material = 99u;
        rejects(snapshot(rows));

        rows = overlap;
        rows[0u].m_mass_kg += 1.0;
        rejects(snapshot(rows));

        rows = overlap;
        rows[0u].m_temperature_k = std::numeric_limits<double>::quiet_NaN();
        rejects(snapshot(rows));

        rows = overlap;
        rows[0u].m_x_m = 64.25;
        rejects(snapshot(rows));

        auto invalid_header = original;
        invalid_header.m_chunks.push_back(original.m_chunks[0u]);
        rejects(invalid_header);

        invalid_header = original;
        invalid_header.m_chunks[0u].m_particles.reset();
        rejects(invalid_header);
    };

    PPR_UNIT_TEST (terrain_halo_uses_shared_seam_masks_and_protected_exterior) {
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({127u, 127u}, sim::Cell{.m_element = 2u}));
        PPR_TEST_ASSERT(not grid.setCell({128u, 128u}, sim::Cell{.m_element = 2u}));
        PPR_TEST_ASSERT(not grid.setCell({128u, 127u}, sim::Cell{.m_element = 9u}));
        const std::array<u16, 1u> solids{2u};

        const auto masks = colony::makeMatterTerrain(grid, solids);
        PPR_TEST_ASSERT(masks and masks->size() == 4097u * 4097u);
        PPR_TEST_ASSERT((*masks)[128u * 4097u + 128u] == 5u);
        PPR_TEST_ASSERT((*masks)[0u] == 11u);
        PPR_TEST_ASSERT((*masks)[4096u * 4097u + 4096u] == 14u);
        PPR_TEST_ASSERT((*masks)[20u * 4097u + 20u] == 0u);
    };

    PPR_UNIT_TEST (body_prediction_preserves_source_motion_units) {
        physics::BodyBoundarySnapshot body{};
        body.m_handle = {10u, 91u};
        body.m_state = {2.0f, 4.0f, 0.0f, 3.0f, -2.0f, 0.5f};
        body.m_com_x = 2.0f;
        body.m_com_y = 4.0f;
        body.m_half_width = 0.5f;
        body.m_half_height = 1.0f;
        body.m_motion = physics::EMotion::dynamic;
        const std::array bodies{body};
        colony::MatterState state{};
        state.m_cell_metres = 0.5;
        state.m_thickness_metres = 2.0;

        const auto predicted = colony::predictMatterBodies(bodies, 0.5, state.m_profile);
        PPR_TEST_ASSERT(predicted and predicted->size() == 8u);
        PPR_TEST_ASSERT(
            (*predicted)[0u].m_com_x ==
            static_cast<float>(static_cast<double>(2.0f + 3.0f * state.m_profile.m_delta_time) / 0.5 + 3.0));
        PPR_TEST_ASSERT((*predicted)[7u].m_com_x != (*predicted)[0u].m_com_x);
        PPR_TEST_ASSERT((*predicted)[0u].m_shape.m_rotation_degrees < 0.0f);

        PPR_TEST_ASSERT((*predicted)[0u].m_velocity_x == body.m_state.m_velocity_x / 0.5f);
        PPR_TEST_ASSERT((*predicted)[0u].m_velocity_y == body.m_state.m_velocity_y / 0.5f);
        PPR_TEST_ASSERT((*predicted)[0u].m_angular_velocity == body.m_state.m_angular_velocity);
    };

    // ------------------------------------------------------------------
    // actual-domain pending driver admission
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (dedicated_actual_domain_cold_reused_acknowledgment_and_completion_are_distinct) {
        colony::test_fixture::AuthoredFixture authored;

        auto shaders = IShaderService::get();
        auto rhi_service = IRhiService::get();
        RenderLane lane;
        colony::MatterRuntime runtime;
        std::counting_semaphore<> wake{0};
        std::optional<colony::MatterRequest> outstanding;
        bool runtime_closed = false;
        PPR_TEST_ASSERT(not shaders->initialize());
        rhi_service->setCoreValidation(true);
        rhi_service->setGpuAssistedValidation(true);
        PPR_TEST_ASSERT(not rhi_service->initialize(kFixtureBackend, *shaders));
        PPR_TEST_ASSERT(not lane.start({}, {}, {}, [&] { wake.release(); }));

        const auto await = [&](Expected<colony::MatterRequest> request) {
            PPR_TEST_ASSERT(request.has_value());
            outstanding = *request;
            for (;;) {
                auto completion = runtime.advance(*outstanding);
                PPR_TEST_ASSERT(completion.has_value());
                if (*completion) {
                    outstanding.reset();
                    return std::move(**completion);
                }

                PPR_TEST_ASSERT(wake.try_acquire_for(std::chrono::seconds{60}));
            }
        };
        const auto success = [&](Expected<colony::MatterRequest> request) {
            auto completion = await(std::move(request));
            PPR_TEST_ASSERT(completion.m_execution.m_ran);
            PPR_TEST_ASSERT(not completion.m_execution.m_operation_error);
            PPR_TEST_ASSERT(not completion.m_execution.m_transport_error);
            return completion;
        };

        // The test owns this queue; it must outlive runtime shutdown and be
        // released before RHI teardown (device-owned children keep the device
        // alive otherwise).
        rhi::ComPtr<rhi::ICommandQueue> queue{};
        PPR_DEFER {
            if (outstanding) {
                std::ignore = await(Expected<colony::MatterRequest>{*outstanding});
            }

            if (not runtime_closed) {
                std::ignore = success(runtime.beginShutdown());
            }

            lane.requestStop();
            PPR_TEST_ASSERT(not lane.join());
            queue.setNull();
            PPR_TEST_ASSERT(not rhi_service->shutdown());
            PPR_TEST_ASSERT(not shaders->shutdown());
        };

        queue = acquireGraphicsQueue(rhi_service->getDevice());
        std::ignore = success(runtime.beginInitialize(
            &lane, rhi_service->getDevice(), *queue, *shaders, std::filesystem::current_path()));

        // A retained admission-busy request owns one input and can be cancelled
        // before acquiring the shared slot. Wrong and consumed tickets reject.
        std::binary_semaphore entered{0}, release{0};
        bool released = false;
        PPR_DEFER {
            if (not released) {
                release.release();
            }
        };
        auto operation = std::make_unique<RenderLaneOperation>([&]() -> std::error_code {
            entered.release();
            release.acquire();
            return {};
        });
        auto blocker = lane.tryBeginOperation(operation);
        PPR_TEST_ASSERT(blocker.has_value());
        PPR_TEST_ASSERT(entered.try_acquire_for(std::chrono::seconds{30}));

        auto cancelled = runtime.beginCancelWorld();
        PPR_TEST_ASSERT(cancelled.has_value());
        auto pending = runtime.advance(*cancelled);
        PPR_TEST_ASSERT(pending and not *pending and not runtime.serviceReady());
        colony::MatterRuntime other_runtime;
        PPR_TEST_ASSERT(not other_runtime.advance(*cancelled));
        PPR_TEST_ASSERT(not runtime.cancel(*cancelled));
        auto cancellation = runtime.advance(*cancelled);
        PPR_TEST_ASSERT(cancellation and *cancellation);
        PPR_TEST_ASSERT(not (**cancellation).m_execution.m_ran);
        PPR_TEST_ASSERT((**cancellation).m_execution.m_cancel_requested);
        PPR_TEST_ASSERT(not runtime.advance(*cancelled));

        release.release();
        released = true;
        for (;;) {
            auto completion = lane.takeOperation(*blocker);
            PPR_TEST_ASSERT(completion.has_value());
            if (*completion) {
                break;
            }

            PPR_TEST_ASSERT(wake.try_acquire_for(std::chrono::seconds{30}));
        }

        colony::ColonyDriver candidate_driver;
        PPR_TEST_ASSERT(not authored.initialize(candidate_driver, {.m_world_generation = 90u}));
        for (;;) {
            auto prepared = candidate_driver.advancePrepareMatter(runtime);
            PPR_TEST_ASSERT(prepared.has_value());
            if (*prepared) {
                break;
            }

            PPR_TEST_ASSERT(wake.try_acquire_for(std::chrono::seconds{30}));
        }

        // Cleanup admission failure retains the prepared owner. The unrelated
        // acknowledged request must settle before terminal discard can be admitted.
        auto foreign_request = runtime.beginDrain();
        PPR_TEST_ASSERT(foreign_request.has_value());
        std::error_code terminal_error{};
        const auto busy_cleanup = candidate_driver.advanceTerminalDetachMatter(terminal_error);
        PPR_TEST_ASSERT(not busy_cleanup and busy_cleanup.error() == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(candidate_driver.matterDeviceOwned());
        PPR_TEST_ASSERT(not terminal_error);
        std::ignore = success(Expected<colony::MatterRequest>{*foreign_request});

        std::binary_semaphore cancel_entered{0}, cancel_release{0};
        bool cancel_released = false;
        PPR_DEFER {
            if (not cancel_released) {
                cancel_release.release();
            }
        };
        auto cancel_blocker_operation = std::make_unique<RenderLaneOperation>([&]() -> std::error_code {
            cancel_entered.release();
            cancel_release.acquire();
            return {};
        });
        auto cancel_blocker = lane.tryBeginOperation(cancel_blocker_operation);
        PPR_TEST_ASSERT(cancel_blocker.has_value());
        PPR_TEST_ASSERT(cancel_entered.try_acquire_for(std::chrono::seconds{30}));

        for (u32 turn = 0u; turn < 2u; ++turn) {
            const auto detached = candidate_driver.advanceTerminalDetachMatter(terminal_error);
            PPR_TEST_ASSERT(detached and not *detached);
            PPR_TEST_ASSERT(candidate_driver.matterDeviceOwned());
            PPR_TEST_ASSERT(candidate_driver.matterServicePending());
            PPR_TEST_ASSERT(not terminal_error);
        }

        cancel_release.release();
        cancel_released = true;
        for (;;) {
            auto completion = lane.takeOperation(*cancel_blocker);
            PPR_TEST_ASSERT(completion.has_value());
            if (*completion) {
                break;
            }

            PPR_TEST_ASSERT(wake.try_acquire_for(std::chrono::seconds{30}));
        }

        for (;;) {
            auto detached = candidate_driver.advanceTerminalDetachMatter(terminal_error);
            PPR_TEST_ASSERT(detached.has_value());
            if (*detached) {
                break;
            }

            PPR_TEST_ASSERT(wake.try_acquire_for(std::chrono::seconds{30}));
        }

        PPR_TEST_ASSERT(not candidate_driver.matterDeviceOwned());
        PPR_TEST_ASSERT(not candidate_driver.matterServicePending() and not terminal_error);
        PPR_TEST_ASSERT(candidate_driver.update(TimeSpan{}) == std::errc::operation_not_permitted);
        const auto reuse = candidate_driver.advancePrepareMatter(runtime);
        PPR_TEST_ASSERT(not reuse and reuse.error() == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not candidate_driver.captureReady());
        PPR_TEST_ASSERT(not candidate_driver.shutdown());

        // Terminal Driver detachment releases its references; the Runtime still
        // owns the prepared candidate until its separate owned cancellation.
        std::ignore = success(runtime.beginCancelWorld());

        auto state = colony::makeMatterState({}, 1.0, 1.0, colony::kMatterGravityY, 91u);
        PPR_TEST_ASSERT(state.has_value());
        state->m_identity.m_terrain_revision = 1u;
        Array<u32> terrain(static_cast<std::size_t>(colony::kMatterTerrainEdge) * colony::kMatterTerrainEdge, 0u);
        std::ignore = success(runtime.beginPrepareResidentWorld(*state, std::move(terrain)));
        std::ignore = success(runtime.beginPublishWorld(state->m_identity));

        for (u32 tick = 0u; tick < 2u; ++tick) {
            const auto start = std::chrono::steady_clock::now();
            const auto submitted = success(runtime.beginSubmitResident(state->m_identity, {}, 0u));
            PPR_TEST_ASSERT(not submitted.m_readback);
            const auto acknowledged_at = std::chrono::steady_clock::now();
            std::optional<colony::MatterReadback> readback;
            while (not readback) {
                auto collected = success(runtime.beginPoll(state->m_identity));
                readback = std::move(collected.m_readback);
                if (not readback) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            PPR_TEST_ASSERT(readback->m_source == state->m_identity);
            PPR_TEST_ASSERT(readback->m_accounting.m_substeps == matter::kGameSubsteps);
            PPR_TEST_ASSERT(readback->m_particles.empty());
            std::fprintf(
                stderr,
                "[matter-dedicated] tick=%u G=4103 substeps=%u ack_ms=%.9g collect_ms=%.9g epoch=%llu bytes=%llu\n",
                tick, readback->m_accounting.m_substeps,
                std::chrono::duration<double, std::milli>(acknowledged_at - start).count(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acknowledged_at).count(),
                static_cast<unsigned long long>(readback->m_accounting.m_epoch),
                static_cast<unsigned long long>(readback->m_accounting.m_device_bytes));

            std::ignore = success(runtime.beginPublish(state->m_identity));
            ++state->m_identity.m_publication;
        }

        std::ignore = success(runtime.beginShutdown());
        runtime_closed = true;
    };

    PPR_UNIT_TEST (selected_map_edges_run_resident_ticks_and_frozen_snapshots) {
        auto shaders = IShaderService::get();
        auto rhi_service = IRhiService::get();
        colony::MatterRuntime runtime;
        bool shaders_attempted = false, rhi_attempted = false, cleaned = false;
        rhi::ComPtr<rhi::ICommandQueue> queue{};
        PPR_DEFER {
            if (not cleaned) {
                // Best-effort assertion-unwind cleanup must not throw another
                // test assertion. Retain dependencies if GPU release is unknown.
                auto request = runtime.beginShutdown();
                bool stopped = false;
                if (request) {
                    auto completion = runtime.advance(*request);
                    stopped = completion and
                              completion->has_value() and
                              not (**completion).m_execution.m_operation_error and
                              not (**completion).m_execution.m_transport_error;
                }

                if (stopped) {
                    queue.setNull();
                    const auto rhi_error = rhi_attempted ? rhi_service->shutdown() : std::error_code{};
                    const auto shader_error = shaders_attempted ? shaders->shutdown() : std::error_code{};
                    if (rhi_error or shader_error) {
                        std::fprintf(
                            stderr, "[map-runtime-cleanup] dependency teardown failed: rhi=%d shader=%d\n",
                            rhi_error.value(), shader_error.value());
                    }
                } else {
                    std::fprintf(stderr, "[map-runtime-cleanup] runtime release unknown; dependencies retained\n");
                }
            }
        };

        shaders_attempted = true;
        PPR_TEST_ASSERT(not shaders->initialize());
        rhi_service->setCoreValidation(true);
        rhi_service->setGpuAssistedValidation(true);
        rhi_attempted = true;
        PPR_TEST_ASSERT(not rhi_service->initialize(kFixtureBackend, *shaders));

        const auto success = [&](Expected<colony::MatterRequest> request) {
            auto completion = completeInline(runtime, std::move(request));
            PPR_TEST_ASSERT(not completion.m_execution.m_operation_error);
            return completion;
        };

        queue = acquireGraphicsQueue(rhi_service->getDevice());
        std::ignore = success(runtime.beginInitialize(
            nullptr, rhi_service->getDevice(), *queue, *shaders, std::filesystem::current_path()));
        std::optional<float> first_displacement;

        for (const u32 edge: {1024u, 2048u, 4096u}) {
            constexpr double cell_metres = 1.0 / 3.0;
            constexpr double volume = cell_metres * cell_metres;
            Array<sim::MatterParticle> rows{
                {1u, 32.5 * cell_metres, 32.5 * cell_metres, volume, 1000.0 * volume, 300.0, 1000.0, 2u,
                    sim::EGenerationMatter::solid},
                {2u, 40.5 * cell_metres, 32.5 * cell_metres, volume, 1000.0 * volume, 300.0, 1000.0, 2u,
                    sim::EGenerationMatter::solid}};
            sim::MatterParticleSnapshot seed{.m_next_id = 3u};
            seed.m_chunks.push_back({{0u, 0u}, std::make_shared<const Array<sim::MatterParticle>>(std::move(rows))});
            auto source = colony::makeMatterState(seed, cell_metres, 1.0, colony::kMatterGravityY, edge, edge);
            PPR_TEST_ASSERT(source.has_value());
            source->m_identity.m_terrain_revision = 1u;

            sim::ChunkGrid grid{edge};
            auto terrain = colony::makeMatterTerrain(grid, {});
            PPR_TEST_ASSERT(terrain.has_value());
            PPR_TEST_ASSERT(terrain->size() == static_cast<std::size_t>(edge + 1u) * (edge + 1u));
            std::ignore = success(runtime.beginPrepareResidentWorld(*source, *terrain));
            auto attached = success(runtime.beginPublishWorld(source->m_identity));
            PPR_TEST_ASSERT(attached.m_view and attached.m_view->current());
            PPR_TEST_ASSERT(attached.m_view->m_metadata->m_count == 2u);
            PPR_TEST_ASSERT(attached.m_view->m_metadata->m_moving_count == 2u);

            std::ignore = success(runtime.beginSubmitResident(source->m_identity, {}, 0u));
            std::ignore = success(runtime.beginDrain());
            auto collected = success(runtime.beginPoll(source->m_identity));
            PPR_TEST_ASSERT(collected.m_readback.has_value());
            PPR_TEST_ASSERT(collected.m_readback->m_particles.empty());
            PPR_TEST_ASSERT(collected.m_readback->m_accounting.m_substeps == matter::kGameSubsteps);
            PPR_TEST_ASSERT(collected.m_readback->m_accounting.m_last_readback_bytes == 24u);
            PPR_TEST_ASSERT(collected.m_readback->m_accounting.m_last_particle_upload_bytes == 0u);

            auto published = success(runtime.beginPublish(source->m_identity));
            ++source->m_identity.m_publication;
            PPR_TEST_ASSERT(published.m_view and published.m_view->current());
            PPR_TEST_ASSERT(not attached.m_view->current());
            PPR_TEST_ASSERT(published.m_view->m_publication == source->m_identity.m_publication);

            std::ignore = success(runtime.beginSnapshot(source->m_identity));
            std::ignore = success(runtime.beginDrain());
            auto snapshot = success(runtime.beginPollSnapshot(source->m_identity));
            PPR_TEST_ASSERT(snapshot.m_readback and snapshot.m_readback->m_source == source->m_identity);
            PPR_TEST_ASSERT(snapshot.m_readback->m_particles.size() == 2u);
            const auto &particle = snapshot.m_readback->m_particles.front();
            PPR_TEST_ASSERT(std::isfinite(particle.m_displacement_y) and particle.m_displacement_y < 0.0f);
            if (first_displacement) {
                PPR_TEST_ASSERT(std::abs(particle.m_displacement_y - *first_displacement) < 0.000001f);
            } else {
                first_displacement = particle.m_displacement_y;
            }

            const auto physical = colony::captureMovingMatter(*source, snapshot.m_readback->m_particles);
            PPR_TEST_ASSERT(physical.has_value());
            PPR_TEST_ASSERT(physical->m_next_id == seed.m_next_id and physical->m_chunks.size() == 1u);
            std::ignore = success(runtime.beginEndSnapshot(source->m_identity));

            // Keep the accepted terrain and all physical records valid while
            // isolating GPU-row admission at the selected solver's upper bound.
            auto invalid_gpu_row = *source;
            invalid_gpu_row.m_particles.front().m_position_x = static_cast<float>(edge) + 8.0f;
            const auto rejected_gpu_row = completeInline(
                runtime, runtime.beginPrepareResidentWorld(invalid_gpu_row, *terrain));
            PPR_TEST_ASSERT(rejected_gpu_row.m_execution.m_operation_error == std::errc::invalid_argument);
            PPR_TEST_ASSERT(published.m_view->current());

            // A coherent record/row pair just beyond the physical map still
            // fits the solver padding. Only the selected world bound rejects it.
            auto invalid_physical_record = *source;
            auto &outside_record = invalid_physical_record.m_records.front();
            auto &outside_row = invalid_physical_record.m_particles.front();
            outside_record.m_x_m = (static_cast<double>(edge) + 0.5) * cell_metres;
            outside_row.m_position_x = static_cast<float>(
                outside_record.m_x_m / cell_metres + static_cast<double>(colony::kMatterOrigin));
            PPR_TEST_ASSERT(outside_row.m_position_x < static_cast<float>(source->m_profile.m_grid_width - 3u));
            PPR_TEST_ASSERT(
                outside_row.m_position_x ==
                static_cast<float>(outside_record.m_x_m / cell_metres + static_cast<double>(colony::kMatterOrigin)));

            const auto rejected_physical_record = completeInline(
                runtime, runtime.beginPrepareResidentWorld(invalid_physical_record, std::move(*terrain)));
            PPR_TEST_ASSERT(rejected_physical_record.m_execution.m_operation_error == std::errc::invalid_argument);
            PPR_TEST_ASSERT(published.m_view->current());

            std::fprintf(
                stderr, "[map-resident] edge=%u grid=%u particles=%zu dy=%.9g epoch=%llu\n", edge,
                source->m_profile.m_grid_width, snapshot.m_readback->m_particles.size(),
                static_cast<double>(particle.m_displacement_y),
                static_cast<unsigned long long>(snapshot.m_readback->m_accounting.m_epoch));
        }

        std::ignore = success(runtime.beginShutdown());
        // The test owns this graphics queue. Release it before RHI teardown so
        // no test-owned device child keeps the device alive past shutdown.
        queue.setNull();
        PPR_TEST_ASSERT(not rhi_service->shutdown());
        PPR_TEST_ASSERT(not shaders->shutdown());
        cleaned = true;
    };

    PPR_UNIT_TEST (resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot) {
        auto shaders = IShaderService::get();
        auto rhi_service = IRhiService::get();
        colony::MatterRuntime runtime;
        PPR_TEST_ASSERT(not shaders->initialize());
        rhi_service->setCoreValidation(true);
        rhi_service->setGpuAssistedValidation(true);
        PPR_TEST_ASSERT(not rhi_service->initialize(kFixtureBackend, *shaders));
        rhi::ComPtr<rhi::ICommandQueue> queue{};
        PPR_DEFER {
            PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginShutdown()).m_execution.m_operation_error);
            queue.setNull();
            PPR_TEST_ASSERT(not rhi_service->shutdown());
            PPR_TEST_ASSERT(not shaders->shutdown());
        };

        const auto execute = [&](Expected<colony::MatterRequest> request) {
            PPR_TEST_ASSERT(request.has_value());
            auto completion = runtime.advance(*request);
            PPR_TEST_ASSERT(completion and completion->has_value());
            PPR_TEST_ASSERT((**completion).m_execution.m_ran);
            PPR_TEST_ASSERT(not (**completion).m_execution.m_transport_error);
            return std::move(**completion);
        };

        const auto success = [&](Expected<colony::MatterRequest> request) {
            auto completion = execute(std::move(request));
            PPR_TEST_ASSERT(not completion.m_execution.m_operation_error);
            return completion;
        };

        queue = acquireGraphicsQueue(rhi_service->getDevice());
        std::ignore = success(runtime.beginInitialize(
            nullptr, rhi_service->getDevice(), *queue, *shaders, std::filesystem::current_path()));
        auto source = colony::makeMatterState(records(), 1.0, 1.0, colony::kMatterGravityY, 94u);
        PPR_TEST_ASSERT(source.has_value());
        source->m_identity.m_terrain_revision = 1u;
        const Array<matter::GpuParticle> seeded_particles = source->m_particles;
        Array<u32> terrain(static_cast<std::size_t>(colony::kMatterTerrainEdge) * colony::kMatterTerrainEdge, 0u);
        std::ignore = success(runtime.beginPrepareResidentWorld(*source, std::move(terrain)));
        auto attached = success(runtime.beginPublishWorld(source->m_identity));
        PPR_TEST_ASSERT(attached.m_view.has_value());
        const auto metadata = attached.m_view->m_metadata;
        PPR_TEST_ASSERT(metadata->m_count == 3u and metadata->m_moving_count == 2u);

        std::ignore = success(runtime.beginSnapshot(source->m_identity));
        const auto frozen = execute(runtime.beginSubmitResident(source->m_identity, {}, 0u));
        PPR_TEST_ASSERT(frozen.m_execution.m_operation_error == std::errc::device_or_resource_busy);
        std::ignore = success(runtime.beginDrain());
        auto initial = success(runtime.beginPollSnapshot(source->m_identity));
        PPR_TEST_ASSERT(initial.m_readback and initial.m_readback->m_source == source->m_identity);
        PPR_TEST_ASSERT(initial.m_readback->m_particles.size() == source->m_particles.size());
        PPR_TEST_ASSERT(
            std::memcmp(
                initial.m_readback->m_particles.data(), source->m_particles.data(),
                source->m_particles.size() * sizeof(matter::GpuParticle)) == 0);

        const auto copied_but_frozen = execute(runtime.beginSubmitResident(source->m_identity, {}, 0u));
        PPR_TEST_ASSERT(copied_but_frozen.m_execution.m_operation_error == std::errc::device_or_resource_busy);
        std::ignore = success(runtime.beginEndSnapshot(source->m_identity));

        std::ignore = success(runtime.beginSubmitResident(source->m_identity, {}, 0u));
        std::ignore = success(runtime.beginDrain());
        auto collected = success(runtime.beginPoll(source->m_identity));
        PPR_TEST_ASSERT(collected.m_readback.has_value());
        PPR_TEST_ASSERT(collected.m_readback->m_particles.empty());
        PPR_TEST_ASSERT(collected.m_readback->m_accounting.m_last_readback_bytes == 24u);
        PPR_TEST_ASSERT(collected.m_readback->m_accounting.m_last_particle_upload_bytes == 0u);

        auto published = success(runtime.beginPublish(source->m_identity));
        ++source->m_identity.m_publication;
        PPR_TEST_ASSERT(published.m_view and published.m_view->m_metadata == metadata);
        PPR_TEST_ASSERT(published.m_view->m_publication == source->m_identity.m_publication);
        PPR_TEST_ASSERT(not attached.m_view->current() and published.m_view->current());

        // Temperature-only same-count replacement: the independent metadata
        // version advances, old retained readers still read their own packed
        // temperature, and the numerical row layout is unchanged.
        PPR_TEST_ASSERT(published.m_view->m_stride == kGridParticleNumericalStride);
        PPR_TEST_ASSERT(published.m_view->m_readonly_stride == kGridParticleReadonlyStride);
        PPR_TEST_ASSERT(published.m_view->m_numerical->getDesc().elementSize == kGridParticleNumericalStride);
        PPR_TEST_ASSERT(published.m_view->m_readonly->getDesc().elementSize == kGridParticleReadonlyStride);
        const Array<GridParticleMetadataRow> old_rows = readMetadata(
            rhi_service->getDevice(), *queue, *published.m_view->m_metadata);
        const u64 old_version = published.m_view->m_metadata->m_version;
        for (const auto &record: source->m_records) {
            const auto found = std::ranges::find_if(
                old_rows, [&](const auto &row) { return (u64{row.m_id_high} << 32u | row.m_id_low) == record.m_id; });
            PPR_TEST_ASSERT(found != old_rows.end());
            PPR_TEST_ASSERT(
                found->m_material_temperature ==
                *lighting::packCell(static_cast<u16>(record.m_material), record.m_temperature_k));
        }

        const double old_authoritative_temperature = source->m_records[0u].m_temperature_k;
        auto candidate = *source;
        candidate.m_records[0u].m_temperature_k = 900.0;
        candidate.m_gas[0u].m_temperature_k = 400.0;
        candidate.m_identity.m_publication = source->m_identity.m_publication + 1u;
        std::ignore = success(runtime.beginPrepareEdit(
            source->m_identity, candidate, static_cast<u32>(source->m_particles.size()), {},
            colony::EMatterEditKind::particle_replace));
        std::ignore = success(runtime.beginPollEdit(candidate.m_identity));
        auto replaced = success(runtime.beginPublishEdit(candidate.m_identity));
        PPR_TEST_ASSERT(replaced.m_view and replaced.m_view->m_metadata);
        PPR_TEST_ASSERT(replaced.m_view->m_metadata != published.m_view->m_metadata);
        PPR_TEST_ASSERT(replaced.m_view->m_metadata->m_version > old_version);
        PPR_TEST_ASSERT(replaced.m_view->m_stride == kGridParticleNumericalStride);
        PPR_TEST_ASSERT(replaced.m_view->m_readonly_stride == kGridParticleReadonlyStride);
        PPR_TEST_ASSERT(source->m_records[0u].m_temperature_k == old_authoritative_temperature);

        const Array<GridParticleMetadataRow> new_rows = readMetadata(
            rhi_service->getDevice(), *queue, *replaced.m_view->m_metadata);
        PPR_TEST_ASSERT(new_rows.size() == old_rows.size());
        for (const auto &record: candidate.m_records) {
            const auto found = std::ranges::find_if(
                new_rows, [&](const auto &row) { return (u64{row.m_id_high} << 32u | row.m_id_low) == record.m_id; });
            PPR_TEST_ASSERT(found != new_rows.end());
            PPR_TEST_ASSERT(
                found->m_material_temperature ==
                *lighting::packCell(static_cast<u16>(record.m_material), record.m_temperature_k));
        }

        const Array<GridParticleMetadataRow> retained_rows = readMetadata(
            rhi_service->getDevice(), *queue, *published.m_view->m_metadata);
        PPR_TEST_ASSERT(std::ranges::equal(retained_rows, old_rows, [](const auto &left, const auto &right) {
            return left.m_material_temperature == right.m_material_temperature;
        }));
        PPR_TEST_ASSERT(not published.m_view->current());
        *source = candidate;

        // Append carries a new stable ID with its real packed temperature.
        auto appended = *source;
        appended.m_records.push_back({40u, 0.75, 0.75, 0.5, 750.0, 555.0, 1500.0, 5u, sim::EGenerationMatter::solid});
        matter::GpuParticle appended_particle{};
        appended_particle.m_position_x = 3.75f;
        appended_particle.m_position_y = 3.75f;
        appended_particle.m_mass = 1.0f;
        appended_particle.m_volume = 1.0f;
        appended_particle.m_material = static_cast<float>(matter::ESourceMaterial::sand);
        appended.m_particles.push_back(appended_particle);
        appended.m_profile.m_particle_count = static_cast<u32>(appended.m_particles.size());
        appended.m_identity.m_publication = source->m_identity.m_publication + 1u;
        std::ignore = success(runtime.beginPrepareEdit(
            source->m_identity, appended, static_cast<u32>(source->m_particles.size()), {},
            colony::EMatterEditKind::particle_append));
        std::ignore = success(runtime.beginPollEdit(appended.m_identity));
        auto appended_view = success(runtime.beginPublishEdit(appended.m_identity));
        PPR_TEST_ASSERT(appended_view.m_view and appended_view.m_view->m_metadata);
        PPR_TEST_ASSERT(appended_view.m_view->m_metadata->m_version > replaced.m_view->m_metadata->m_version);
        PPR_TEST_ASSERT(not replaced.m_view->current());
        const Array<GridParticleMetadataRow> appended_rows = readMetadata(
            rhi_service->getDevice(), *queue, *appended_view.m_view->m_metadata);
        const auto appended_row = std::ranges::find_if(
            appended_rows, [&](const auto &row) { return (u64{row.m_id_high} << 32u | row.m_id_low) == 40u; });
        PPR_TEST_ASSERT(appended_row != appended_rows.end());
        PPR_TEST_ASSERT(appended_row->m_material_temperature == *lighting::packCell(5u, 555.0));
        const auto retained_id10 = std::ranges::find_if(
            appended_rows, [&](const auto &row) { return (u64{row.m_id_high} << 32u | row.m_id_low) == 10u; });
        PPR_TEST_ASSERT(retained_id10 != appended_rows.end());
        PPR_TEST_ASSERT(retained_id10->m_material_temperature == *lighting::packCell(1u, 900.0));
        *source = appended;

        // Stable-ID ordering survives a numerical-row reorder: metadata rows
        // stay sorted by ID while m_source_row names the committed solver row.
        auto reordered = *source;
        std::swap(reordered.m_records[0u], reordered.m_records[1u]);
        std::swap(reordered.m_particles[0u], reordered.m_particles[1u]);
        reordered.m_identity.m_publication = source->m_identity.m_publication + 1u;
        std::ignore = success(runtime.beginPrepareEdit(
            source->m_identity, reordered, static_cast<u32>(source->m_particles.size()), {},
            colony::EMatterEditKind::particle_replace));
        std::ignore = success(runtime.beginPollEdit(reordered.m_identity));
        auto reordered_view = success(runtime.beginPublishEdit(reordered.m_identity));
        PPR_TEST_ASSERT(reordered_view.m_view and reordered_view.m_view->m_metadata);
        PPR_TEST_ASSERT(not appended_view.m_view->current());
        const Array<GridParticleMetadataRow> reordered_rows = readMetadata(
            rhi_service->getDevice(), *queue, *reordered_view.m_view->m_metadata);
        u64 previous_id{};
        for (const auto &row: reordered_rows) {
            const u64 id = u64{row.m_id_high} << 32u | row.m_id_low;
            PPR_TEST_ASSERT(id > previous_id);
            previous_id = id;
            PPR_TEST_ASSERT(
                (row.m_material_temperature & 0xffffu) > 0u and (row.m_material_temperature & 0xffffu) < 0xffffu);
            if (row.m_source_row == 0xffff'ffffu) {
                continue;
            }
            PPR_TEST_ASSERT(row.m_source_row < reordered.m_particles.size());
            std::array<float, 2u> position{};
            PPR_TEST_ASSERT(not make_error_code(rhi_service->getDevice().readBuffer(
                reordered_view.m_view->m_numerical, static_cast<u64>(row.m_source_row) * kGridParticleNumericalStride,
                sizeof(position), position.data())));
            PPR_TEST_ASSERT(position[0u] == reordered.m_particles[row.m_source_row].m_position_x);
            PPR_TEST_ASSERT(position[1u] == reordered.m_particles[row.m_source_row].m_position_y);
        }
        *source = reordered;

        // Invalid Kelvin rejects before any upload or identity commit.
        const auto rejected_temperature = [&](const double kelvin) {
            auto invalid = *source;
            invalid.m_identity.m_publication = source->m_identity.m_publication + 1u;
            invalid.m_records[0u].m_temperature_k = kelvin;
            auto request = runtime.beginPrepareEdit(
                source->m_identity, invalid, 0u, {}, colony::EMatterEditKind::particle_replace);
            PPR_TEST_ASSERT(not request);
            return request.error();
        };
        PPR_TEST_ASSERT(rejected_temperature(-1.0) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(rejected_temperature(std::numeric_limits<double>::quiet_NaN()) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(rejected_temperature(65'506.0) == std::errc::result_out_of_range);
        PPR_TEST_ASSERT(reordered_view.m_view->current());
        PPR_TEST_ASSERT(source->m_identity.m_publication == reordered.m_identity.m_publication);

        // End before collection drains an already-admitted copy and retains
        // the same committed source. Owned prior bytes survive later ticks.
        std::ignore = success(runtime.beginSnapshot(source->m_identity));
        std::ignore = success(runtime.beginEndSnapshot(source->m_identity));
        PPR_TEST_ASSERT(reordered_view.m_view->current());
        PPR_TEST_ASSERT(initial.m_readback->m_source.m_publication == 0u);
        PPR_TEST_ASSERT(
            std::memcmp(
                initial.m_readback->m_particles.data(), seeded_particles.data(),
                seeded_particles.size() * sizeof(matter::GpuParticle)) == 0);

        // Release view owners before the injected RHI fixture shuts down.
        attached.m_view.reset();
        published.m_view.reset();
        collected.m_view.reset();
        initial.m_view.reset();
    };

    PPR_UNIT_TEST (terminal_collection_rejection_discards_without_joint_publication) {
        colony::test_fixture::AuthoredFixture authored;
        auto shaders = IShaderService::get();
        auto rhi_service = IRhiService::get();
        colony::MatterRuntime runtime;
        colony::ColonyDriver driver;
        PPR_TEST_ASSERT(not shaders->initialize());
        rhi_service->setCoreValidation(true);
        rhi_service->setGpuAssistedValidation(true);
        PPR_TEST_ASSERT(not rhi_service->initialize(kFixtureBackend, *shaders));
        rhi::ComPtr<rhi::ICommandQueue> queue{};
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
            PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginShutdown()).m_execution.m_operation_error);
            queue.setNull();
            PPR_TEST_ASSERT(not rhi_service->shutdown());
            PPR_TEST_ASSERT(not shaders->shutdown());
        };

        queue = acquireGraphicsQueue(rhi_service->getDevice());
        PPR_TEST_ASSERT(not completeInline(
            runtime, runtime.beginInitialize(
                         nullptr, rhi_service->getDevice(), *queue, *shaders, std::filesystem::current_path()))
                .m_execution.m_operation_error);
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_world_generation = 95u}));
        PPR_TEST_ASSERT(not driver.prepareMatter(runtime));
        PPR_TEST_ASSERT(not driver.attachMatter(runtime));
        const auto identity = driver.matterState()->m_identity;
        const auto before = runtime.accounting();
        PPR_TEST_ASSERT(before.has_value());

        const auto injected = std::make_error_code(std::errc::io_error);
        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginInjectCollectionFailureForTest(injected))
                .m_execution.m_operation_error);
        const TimeSpan period = TimeSpan{std::chrono::seconds{1}} / 60;
        PPR_TEST_ASSERT(not driver.update(period));
        PPR_TEST_ASSERT(driver.physicsPending() and driver.matterDeviceOwned());
        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginDrain()).m_execution.m_operation_error);
        PPR_TEST_ASSERT(driver.update(TimeSpan{}) == injected);
        PPR_TEST_ASSERT(driver.physicsPending() and driver.tickCount() == 0u);
        const auto failed = runtime.failure();
        PPR_TEST_ASSERT(failed and failed->m_cause == injected);

        std::error_code first{};
        const auto detached = driver.advanceTerminalDetachMatter(first);
        PPR_TEST_ASSERT(detached and *detached);
        PPR_TEST_ASSERT(first == injected);
        PPR_TEST_ASSERT(not driver.matterDeviceOwned() and not driver.matterServicePending());
        PPR_TEST_ASSERT(not driver.physicsPending() and driver.tickCount() == 0u);
        PPR_TEST_ASSERT(driver.matterState()->m_identity == identity);
        PPR_TEST_ASSERT(not driver.particleView() and not driver.captureReady());
        PPR_TEST_ASSERT(driver.update(period) == std::errc::operation_not_permitted);
        const auto after = runtime.accounting();
        const auto retained = runtime.failure();
        PPR_TEST_ASSERT(after and after->m_epoch == before->m_epoch);
        PPR_TEST_ASSERT(retained and retained->m_cause == injected);
        PPR_TEST_ASSERT(not driver.advanceResumeMatter(runtime));
        PPR_TEST_ASSERT(not driver.advanceMatterSnapshot());
        PPR_TEST_ASSERT(not driver.captureMatterState());
    };

    PPR_UNIT_TEST (actual_domain_pending_pause_finishes_only_admitted_slice) {
        colony::test_fixture::AuthoredFixture authored;

        auto shaders = IShaderService::get();
        auto rhi_service = IRhiService::get();
        RenderLane stopped_lane;
        colony::MatterRuntime runtime;
        colony::ColonyDriver driver;
        bool shaders_attempted = false, rhi_attempted = false;
        rhi::ComPtr<rhi::ICommandQueue> queue{};
        PPR_DEFER {
            const auto driver_error = driver.shutdown();
            const auto runtime_error = completeInline(runtime, runtime.beginShutdown()).m_execution.m_operation_error;
            // RHI/shaders are dependencies, not independent cleanup, when
            // completion is unknown. Retain them rather than release off-lane.
            PPR_TEST_ASSERT(not runtime_error);
            queue.setNull();
            const auto rhi_error = rhi_attempted ? rhi_service->shutdown() : std::error_code{};
            const auto shader_error = shaders_attempted ? shaders->shutdown() : std::error_code{};

            PPR_TEST_ASSERT(not driver_error);
            PPR_TEST_ASSERT(not rhi_error);
            PPR_TEST_ASSERT(not shader_error);
        };

        shaders_attempted = true;
        PPR_TEST_ASSERT(not shaders->initialize());
        rhi_service->setCoreValidation(true);
        rhi_service->setGpuAssistedValidation(true);
        rhi_attempted = true;
        PPR_TEST_ASSERT(not rhi_service->initialize(kFixtureBackend, *shaders));
        PPR_TEST_ASSERT(rhi_service->getDevice().getDeviceType() == kFixtureBackend);
        const auto &device = rhi_service->getDevice().getInfo();
        std::fprintf(
            stderr, "[matter-domain-device] api=%s adapter=%s workload=empty-particle-dense-grid-lifecycle-pause\n",
            device.apiName ? device.apiName : "unavailable", device.adapterName ? device.adapterName : "unavailable");

        queue = acquireGraphicsQueue(rhi_service->getDevice());
        const auto unavailable = runtime.beginInitialize(
            &stopped_lane, rhi_service->getDevice(), *queue, *shaders, std::filesystem::current_path());
        PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::not_connected);

        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginShutdown()).m_execution.m_operation_error);

        PPR_TEST_ASSERT(not completeInline(
            runtime, runtime.beginInitialize(
                         nullptr, rhi_service->getDevice(), *queue, *shaders, std::filesystem::current_path()))
                .m_execution.m_operation_error);

        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_world_generation = 81u}));
        const auto start = std::chrono::steady_clock::now();
        PPR_TEST_ASSERT(not driver.prepareMatter(runtime));
        PPR_TEST_ASSERT(not driver.attachMatter(runtime));
        PPR_TEST_ASSERT(driver.matterState()->m_identity.m_publication == 0u);
        PPR_TEST_ASSERT(driver.matterState()->m_profile.m_grid_width == 4103u);
        const TimeSpan period = TimeSpan{std::chrono::seconds{1}} / 60;
        const auto prepared_at = std::chrono::steady_clock::now();

        // A pre-dispatch rejection still freezes the attempted batch and keeps
        // its original cause visible until an explicit completion/discard.
        const auto identity = driver.matterState()->m_identity;
        auto rejected_submit = runtime.beginSubmitResident(identity, {}, 1u);
        PPR_TEST_ASSERT(rejected_submit.has_value());
        auto rejected_ack = runtime.advance(*rejected_submit);
        PPR_TEST_ASSERT(rejected_ack and *rejected_ack);
        PPR_TEST_ASSERT((**rejected_ack).m_execution.m_operation_error == std::errc::invalid_argument);

        const auto rejected = completeInline(runtime, runtime.beginPoll(identity));
        PPR_TEST_ASSERT(rejected.m_execution.m_operation_error == std::errc::invalid_argument);

        const auto cause = runtime.failure();
        PPR_TEST_ASSERT(cause and cause->m_cause == std::errc::invalid_argument);

        PPR_TEST_ASSERT(
            completeInline(runtime, runtime.beginResumeWorld(identity)).m_execution.m_operation_error ==
            std::errc::operation_not_permitted);

        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginDiscard()).m_execution.m_operation_error);

        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginResumeWorld(identity)).m_execution.m_operation_error);

        const auto tick_started = std::chrono::steady_clock::now();
        PPR_TEST_ASSERT(not driver.update(2 * period));
        const auto submitted_at = std::chrono::steady_clock::now();
        PPR_TEST_ASSERT(driver.physicsPending() and driver.tickCount() == 0u);
        driver.setPaused(true);

        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginDrain()).m_execution.m_operation_error);
        const auto completed_at = std::chrono::steady_clock::now();
        const auto completed = completeInline(runtime, runtime.beginPoll(identity));
        PPR_TEST_ASSERT(not completed.m_execution.m_operation_error);
        PPR_TEST_ASSERT(completed.m_readback.has_value());
        const auto collected_at = std::chrono::steady_clock::now();
        const auto accounting = completed.m_readback->m_accounting;
        PPR_TEST_ASSERT(accounting.m_epoch == 1u and identity.m_publication == 0u);
        std::fprintf(
            stderr,
            "[matter-domain] G=4103 substeps=%u device_bytes=%llu readback_capacity=%llu readback_bytes=%llu prepare_ms=%.9g update_submit_ms=%.9g completion_wait_ms=%.9g collection_ms=%.9g batch_cpu_ms=%.9g initial_uploads_may_share_queue=true\n",
            static_cast<unsigned int>(accounting.m_substeps),
            static_cast<unsigned long long>(accounting.m_device_bytes),
            static_cast<unsigned long long>(accounting.m_readback_capacity_bytes),
            static_cast<unsigned long long>(accounting.m_last_readback_bytes),
            std::chrono::duration<double, std::milli>(prepared_at - start).count(),
            std::chrono::duration<double, std::milli>(submitted_at - tick_started).count(),
            std::chrono::duration<double, std::milli>(completed_at - submitted_at).count(),
            std::chrono::duration<double, std::milli>(collected_at - completed_at).count(),
            std::chrono::duration<double, std::milli>(collected_at - tick_started).count());

        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 1u and not driver.physicsPending());
        PPR_TEST_ASSERT(not driver.captureReady() and not driver.admitsElapsed());
        PPR_TEST_ASSERT(driver.matterState()->m_identity.m_publication == 1u);
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 1u and not driver.physicsPending());
        PPR_TEST_ASSERT(driver.particleCount() == 0u);

        // Resume the preserved slice without admitting elapsed time or replacing
        // the live owner. This observes reuse, not generated-particle physics.
        const auto reused_identity = driver.matterState()->m_identity;
        const auto preserved_next_id = driver.matterState()->m_next_id;
        driver.setPaused(false);
        const auto reused_started = std::chrono::steady_clock::now();
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        const auto reused_submitted_at = std::chrono::steady_clock::now();
        PPR_TEST_ASSERT(driver.tickCount() == 1u and driver.physicsPending());
        PPR_TEST_ASSERT(driver.matterState()->m_identity == reused_identity);
        PPR_TEST_ASSERT(not driver.captureReady() and not driver.admitsElapsed());

        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginDrain()).m_execution.m_operation_error);
        const auto reused_completed_at = std::chrono::steady_clock::now();
        const auto reused_completed = completeInline(runtime, runtime.beginPoll(reused_identity));
        PPR_TEST_ASSERT(not reused_completed.m_execution.m_operation_error);
        PPR_TEST_ASSERT(reused_completed.m_readback.has_value());
        const auto reused_collected_at = std::chrono::steady_clock::now();
        const auto reused_accounting = reused_completed.m_readback->m_accounting;
        PPR_TEST_ASSERT(reused_completed.m_readback->m_source == reused_identity);
        PPR_TEST_ASSERT(reused_completed.m_readback->m_particles.empty());
        PPR_TEST_ASSERT(reused_accounting.m_epoch == accounting.m_epoch + 1u);
        PPR_TEST_ASSERT(reused_accounting.m_substeps == accounting.m_substeps);
        PPR_TEST_ASSERT(reused_accounting.m_device_bytes == accounting.m_device_bytes);
        PPR_TEST_ASSERT(reused_accounting.m_readback_capacity_bytes == accounting.m_readback_capacity_bytes);
        std::fprintf(
            stderr,
            "[matter-domain-reused] G=4103 substeps=%u source_publication=%llu solver_epoch=%llu dispatches=%llu device_bytes=%llu readback_capacity=%llu readback_bytes=%llu update_submit_ms=%.9g completion_wait_ms=%.9g collection_ms=%.9g batch_cpu_ms=%.9g resources_reused=true workload=empty-particle-dense-grid-lifecycle-pause\n",
            static_cast<unsigned int>(reused_accounting.m_substeps),
            static_cast<unsigned long long>(reused_identity.m_publication),
            static_cast<unsigned long long>(reused_accounting.m_epoch),
            static_cast<unsigned long long>(reused_accounting.m_dispatches),
            static_cast<unsigned long long>(reused_accounting.m_device_bytes),
            static_cast<unsigned long long>(reused_accounting.m_readback_capacity_bytes),
            static_cast<unsigned long long>(reused_accounting.m_last_readback_bytes),
            std::chrono::duration<double, std::milli>(reused_submitted_at - reused_started).count(),
            std::chrono::duration<double, std::milli>(reused_completed_at - reused_submitted_at).count(),
            std::chrono::duration<double, std::milli>(reused_collected_at - reused_completed_at).count(),
            std::chrono::duration<double, std::milli>(reused_collected_at - reused_started).count());

        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 2u and not driver.physicsPending());
        PPR_TEST_ASSERT(driver.captureReady() and driver.admitsElapsed());
        PPR_TEST_ASSERT(driver.matterState()->m_identity.m_publication == reused_identity.m_publication + 1u);
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == preserved_next_id);
        PPR_TEST_ASSERT(driver.matterState()->m_particles.empty() and driver.matterState()->m_records.empty());
        PPR_TEST_ASSERT(driver.matterState()->m_gas.empty() and driver.particleCount() == 0u);

        const auto settled_identity = driver.matterState()->m_identity;
        for (u32 repeat = 0u; repeat < 2u; ++repeat) {
            PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
            PPR_TEST_ASSERT(driver.tickCount() == 2u and not driver.physicsPending());
            PPR_TEST_ASSERT(driver.captureReady() and driver.admitsElapsed());
            PPR_TEST_ASSERT(driver.matterState()->m_identity == settled_identity);
        }

        // Preparing another world freezes admission on the current owner. A
        // rejected submission cannot leave flags that migrate to a new solver.
        const auto current_identity = driver.matterState()->m_identity;
        driver.setPaused(true);
        const auto before_replacement = runtime.accounting();
        PPR_TEST_ASSERT(before_replacement.has_value());
        auto replacement = *driver.matterState();
        replacement.m_identity.m_world = 82u;
        Array<u32> replacement_terrain(
            static_cast<std::size_t>(colony::kMatterTerrainEdge) * colony::kMatterTerrainEdge, 0u);
        PPR_TEST_ASSERT(
            not completeInline(runtime, runtime.beginPrepareResidentWorld(replacement, std::move(replacement_terrain)))
                .m_execution.m_operation_error);

        const auto with_replacement = runtime.accounting();
        PPR_TEST_ASSERT(with_replacement and with_replacement->m_device_bytes > before_replacement->m_device_bytes);
        std::fprintf(
            stderr,
            "[matter-prepared] active_device_bytes=%llu active_and_staged_device_bytes=%llu readback_capacity=%llu\n",
            static_cast<unsigned long long>(before_replacement->m_device_bytes),
            static_cast<unsigned long long>(with_replacement->m_device_bytes),
            static_cast<unsigned long long>(with_replacement->m_readback_capacity_bytes));

        PPR_TEST_ASSERT(
            completeInline(runtime, runtime.beginSubmitResident(current_identity, {}, 0u))
                .m_execution.m_operation_error == std::errc::device_or_resource_busy);

        PPR_TEST_ASSERT(not completeInline(runtime, runtime.beginCancelWorld()).m_execution.m_operation_error);

        PPR_TEST_ASSERT(
            not completeInline(runtime, runtime.beginResumeWorld(current_identity)).m_execution.m_operation_error);

        const auto after_replacement = runtime.accounting();
        PPR_TEST_ASSERT(after_replacement and after_replacement->m_device_bytes == before_replacement->m_device_bytes);
        PPR_TEST_ASSERT(after_replacement->m_readback_capacity_bytes == before_replacement->m_readback_capacity_bytes);
        PPR_TEST_ASSERT(driver.tickCount() == 2u and driver.matterState()->m_identity == current_identity);

        PPR_TEST_ASSERT(not driver.detachMatter());
        PPR_TEST_ASSERT(driver.update(TimeSpan{}) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(driver.tickCount() == 2u and driver.worldGeneration() == 81u);
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // adapter and runtime test registration
    // ------------------------------------------------------------------

    const UnitTest matter_adapter = UnitTest::Named("matter_adapter") / [](UnitTest::IRun &_) {
        _.recurse({
            detail::MatterAdapter::source_units_defaults_materials_and_unsimulated_gas_preserve_inventory,
            detail::MatterAdapter::selected_map_edges_preserve_units_and_close_particle_bounds,
            detail::MatterAdapter::moving_records_allow_overlap_detached_solids_and_exact_upper_endpoint,
            detail::MatterAdapter::moving_validation_preserves_static_cell_and_global_identity_guards,
            detail::MatterAdapter::terrain_halo_uses_shared_seam_masks_and_protected_exterior,
            detail::MatterAdapter::body_prediction_preserves_source_motion_units,
        });
    };

    const UnitTest matter_runtime_gpu = UnitTest::Named("matter_runtime_gpu") / [](UnitTest::IRun &_) {
        _.recurse(
            {detail::MatterAdapter::selected_map_edges_run_resident_ticks_and_frozen_snapshots,
                detail::MatterAdapter::terminal_collection_rejection_discards_without_joint_publication,
                detail::MatterAdapter::actual_domain_pending_pause_finishes_only_admitted_slice,
                detail::MatterAdapter::resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot,
                detail::MatterAdapter::dedicated_actual_domain_cold_reused_acknowledgment_and_completion_are_distinct});
    };

    const UnitTest &matter_adapterTests() noexcept {
        return matter_adapter;
    }

    const UnitTest &matter_runtime_gpuTests() noexcept {
        return matter_runtime_gpu;
    }
}
