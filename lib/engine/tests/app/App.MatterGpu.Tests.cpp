module;

#include "pP/UnitTest.h"
#include <cstdio>
#include <slang.h>

module engine.tests.app;

import std;
import engine.core;
import engine.shader;
import engine.rhi;
import engine.matter;
import engine.math;

namespace pP::tests::detail::MatterGpu {
    using namespace pP::matter;

    // ------------------------------------------------------------------
    // explicit headless D3D12 fixture
    // ------------------------------------------------------------------

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue;
        rhi::ComPtr<rhi::IComputePipeline> m_contact_pipeline;
        rhi::ComPtr<rhi::IComputePipeline> m_compare_pipeline;
        rhi::ComPtr<rhi::IComputePipeline> m_wave_compare_pipeline;
        rhi::ComPtr<rhi::IComputePipeline> m_codec_pipeline;
        rhi::ComPtr<rhi::IComputePipeline> m_bootstrap_pipeline;
        GpuSolver m_solver;

        void initialize(
            const bool use_volume = true, const u32 grid_width = 32u, const u32 grid_height = 32u,
            const bool validation = true, const ITimerClock *const observation_clock = nullptr) {
            PPR_TEST_ASSERT(not m_shaders->initialize());
            m_rhi->setCoreValidation(validation);
            m_rhi->setGpuAssistedValidation(validation);
            PPR_TEST_ASSERT(m_rhi->isCoreValidationEnabled() == validation);
            PPR_TEST_ASSERT(m_rhi->isGpuAssistedValidationEnabled() == validation);
            PPR_TEST_ASSERT(not m_rhi->initialize(rhi::DeviceType::D3D12, *m_shaders));
            PPR_TEST_ASSERT(m_rhi->getDevice().getDeviceType() == rhi::DeviceType::D3D12);
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            GpuParameters parameters{};
            parameters.m_grid_width = grid_width;
            parameters.m_grid_height = grid_height;
            parameters.m_use_grid_volume_for_liquid = use_volume ? 1u : 0u;
            PPR_TEST_ASSERT(not m_solver.initialize(
                m_rhi->getDevice(), *m_queue, *m_shaders, std::filesystem::current_path(), parameters,
                observation_clock));
        }

        void shutdown() {
            const std::error_code solver_error = m_solver.shutdown();
            // A failed completion boundary must retain resources, so stop before
            // destroying their injected dependencies in that exceptional case.
            PPR_TEST_ASSERT(not solver_error);
            m_bootstrap_pipeline.setNull();
            m_codec_pipeline.setNull();
            m_wave_compare_pipeline.setNull();
            m_compare_pipeline.setNull();
            m_contact_pipeline.setNull();
            m_queue.setNull();
            const std::error_code rhi_error = m_rhi->shutdown();
            const std::error_code shader_error = m_shaders->shutdown();
            PPR_TEST_ASSERT(not rhi_error);
            PPR_TEST_ASSERT(not shader_error);
        }

        void replace(const std::span<const GpuParticle> particles) {
            PPR_TEST_ASSERT(not m_solver.prepareReplacement(particles, {}, m_solver.accounting().m_epoch));
            m_solver.publishReplacement();
        }

        Array<GpuParticle> snapshot() {
            const u64 epoch = m_solver.accounting().m_epoch;

            PPR_TEST_ASSERT(not m_solver.beginSnapshot(epoch));

            PPR_TEST_ASSERT(not m_solver.drain());

            auto captured = m_solver.pollSnapshot();
            PPR_TEST_ASSERT(captured and captured->has_value());
            PPR_TEST_ASSERT((**captured).m_epoch == epoch);
            return std::move((**captured).m_particles);
        }

        Array<GpuParticle> publishSnapshot(const u64 expected_epoch) {
            const auto previous = m_solver.committedView();
            PPR_TEST_ASSERT(previous and previous->m_epoch == expected_epoch);
            PPR_TEST_ASSERT(previous->m_live_epoch->load() == expected_epoch);
            PPR_TEST_ASSERT(m_solver.accounting().m_epoch == expected_epoch);
            PPR_TEST_ASSERT(m_solver.accounting().m_last_readback_bytes == 6u * sizeof(u32));
            PPR_TEST_ASSERT(m_solver.beginSnapshot(expected_epoch) == std::errc::device_or_resource_busy);

            m_solver.publishTick();
            PPR_TEST_ASSERT(m_solver.accounting().m_epoch == expected_epoch + 1u);
            PPR_TEST_ASSERT(previous->m_live_epoch->load() != previous->m_epoch);

            return snapshot();
        }

        void finishOneSubstep() {
            PPR_TEST_ASSERT(not m_solver.submitResidentTick(m_solver.accounting().m_epoch, 1u, 2u));

            PPR_TEST_ASSERT(not m_solver.drain());

            const auto complete = m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
        }
    };

    GpuParticle restParticle() noexcept {
        GpuParticle particle{};
        particle.m_position_x = 6.5f;
        particle.m_position_y = 6.5f;
        return particle;
    }

    // ------------------------------------------------------------------
    // split device row packing
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (split_device_rows_pack_and_unpack_every_semantic_field_by_name) {
        GpuParticle particle{};
        particle.m_position_x = 3.25f;
        particle.m_position_y = 17.5f;
        particle.m_displacement_x = -0.125f;
        particle.m_displacement_y = 0.375f;
        particle.m_deformation_gradient = {0.5f, -0.25f, 0.75f, 1.25f};
        particle.m_deformation_displacement = {0.0625f, -0.5f, 0.3125f, -0.0625f};
        particle.m_liquid_density = 0.875f;
        particle.m_mass = 2.5f;
        particle.m_material = 2.0f;
        particle.m_volume = 0.03125f;
        particle.m_lambda = -0.75f;
        particle.m_log_jp = 0.125f;
        particle.m_color_r = 0.1875f;
        particle.m_color_g = 0.4375f;
        particle.m_color_b = 0.6875f;
        particle.m_enabled = 1.0f;

        const ParticleWrite hot = packWrite(particle);
        const ParticleReadonly cold = packReadonly(particle);

        PPR_TEST_ASSERT(hot.m_deformation_gradient.m_c0x == 0.5f and hot.m_deformation_gradient.m_c1y == 1.25f);
        PPR_TEST_ASSERT(
            hot.m_deformation_displacement.m_c1x == 0.3125f and hot.m_deformation_displacement.m_c0y == -0.5f);
        PPR_TEST_ASSERT(hot.m_liquid_density == 0.875f and hot.m_log_jp == 0.125f and hot.m_lambda == -0.75f);
        PPR_TEST_ASSERT(hot.m_enabled == 1.0f and cold.m_mass == 2.5f and cold.m_volume == 0.03125f);
        PPR_TEST_ASSERT(cold.m_material == 2.0f and cold.m_color_b == 0.6875f);
        PPR_TEST_ASSERT(sizeof(cold) == 24u and sizeof(hot) == 64u and sizeof(particle) == 88u);

        const GpuParticle restored = unpack(hot, cold);
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 22u>>(restored) == std::bit_cast<std::array<u32, 22u>>(particle));
    };

    PPR_UNIT_TEST (lossless_terrain_codecs_preserve_tables_tails_and_reject_invalid_input) {
        const auto packed = packContourTables(math::contourPoints(), math::contourCases());
        PPR_TEST_ASSERT(packed.has_value());

        for (u32 index = 0u; index < 16u; ++index) {
            const auto restored = unpackContourCase(packed->m_cases[index]);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 18u>>(restored) ==
                std::bit_cast<std::array<u32, 18u>>(math::contourCases()[index]));
            PPR_TEST_ASSERT((packed->m_cases[index].m_high >> 23u) == 0u);
        }

        for (u32 index = 0u; index < 8u; ++index) {
            const auto restored = unpackContourPoint(packed->m_points, index);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 2u>>(restored) ==
                std::bit_cast<std::array<u32, 2u>>(math::contourPoints()[index]));
        }

        for (const u32 width: {1u, 7u, 8u, 9u, 31u, 32u, 33u, 257u, 4097u}) {
            constexpr u32 height = 9u;
            Array<u32> masks(static_cast<std::size_t>(width) * height);
            for (std::size_t index = 0u; index < masks.size(); ++index)
                masks[index] = static_cast<u32>(index % 16u);

            const auto terrain = packTerrain({masks, width, height});
            PPR_TEST_ASSERT(terrain.has_value());
            PPR_TEST_ASSERT(terrain->m_masks.size() == (masks.size() + 7u) / 8u);
            for (std::size_t index = 0u; index < masks.size(); ++index) {
                const u32 restored = (terrain->m_masks[index / 8u] >> (4u * (index % 8u))) & 15u;
                PPR_TEST_ASSERT(restored == masks[index]);
            }

            const u32 mask_tail = static_cast<u32>(masks.size() % 8u) * 4u;
            if (mask_tail != 0u)
                PPR_TEST_ASSERT((terrain->m_masks.back() >> mask_tail) == 0u);

            const u32 columns = (width + 7u) / 8u;
            const u32 tile_count = columns * ((height + 7u) / 8u);
            PPR_TEST_ASSERT(terrain->m_boundary_tiles.size() == (tile_count + 31u) / 32u);
            for (u32 tile = 0u; tile < tile_count; ++tile) {
                bool expected = false;
                for (u32 y = (tile / columns) * 8u; y < std::min(height, (tile / columns + 1u) * 8u); ++y) {
                    for (u32 x = (tile % columns) * 8u; x < std::min(width, (tile % columns + 1u) * 8u); ++x) {
                        const u32 mask = masks[static_cast<std::size_t>(y) * width + x];
                        expected = expected or (mask != 0u and mask != 15u);
                    }
                }

                PPR_TEST_ASSERT(
                    ((terrain->m_boundary_tiles[tile / 32u] >> (tile % 32u)) & 1u) == static_cast<u32>(expected));
            }

            if (tile_count % 32u != 0u) {
                PPR_TEST_ASSERT((terrain->m_boundary_tiles.back() >> (tile_count % 32u)) == 0u);
            }

            masks.back() = 16u;
            const auto invalid = packTerrain({masks, width, height});
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
        }

        const auto empty = packTerrain({});
        PPR_TEST_ASSERT(empty and empty->m_masks.empty() and empty->m_boundary_tiles.empty());
        const auto invalid_empty = packTerrain({{}, 1u, 0u});
        PPR_TEST_ASSERT(not invalid_empty and invalid_empty.error() == std::errc::invalid_argument);
        const std::array<u32, 1u> one_mask{1u};
        const auto invalid_dimensions = packTerrain({one_mask, 2u, 1u});
        PPR_TEST_ASSERT(not invalid_dimensions and invalid_dimensions.error() == std::errc::invalid_argument);

        std::array<math::ContourPoint, 8u> points;
        std::ranges::copy(math::contourPoints(), points.begin());
        points[0u].m_x = 0.25f;
        const auto invalid_point = packContourTables(points, math::contourCases());
        PPR_TEST_ASSERT(not invalid_point and invalid_point.error() == std::errc::invalid_argument);

        for (u32 invalid_field = 0u; invalid_field < 4u; ++invalid_field) {
            std::array<math::ContourCase, 16u> cases;
            std::ranges::copy(math::contourCases(), cases.begin());
            if (invalid_field == 0u)
                cases[0u].m_segment_count = 3u;
            if (invalid_field == 1u)
                cases[0u].m_triangle_count = 4u;
            if (invalid_field == 2u)
                cases[0u].m_segments[1u].m_end = 8u;
            if (invalid_field == 3u)
                cases[0u].m_triangles[2u].m_solid_corners = 16u;

            const auto invalid = packContourTables(math::contourPoints(), cases);
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
        }
    };

    // ------------------------------------------------------------------
    // fixed single-node execution of the production grid-contact entry
    // ------------------------------------------------------------------

    struct GridResult {
        Array<u32> m_wave_contacts;
        std::array<i32, 4u> m_node{};
        std::array<u32, 6u> m_errors{};
        std::array<u32, 12u> m_contacts{};
        std::array<u32, 64u> m_codec{};
        bool m_bootstrap_certified{};
    };

    GridResult contactProbe(
        Fixture &fixture, const TerrainView terrain, const std::span<const GpuBodyBoundary> bodies,
        const float displacement_y, const std::span<const GpuStaticShape> shapes = {},
        const i32 encoded_mass = 10'000'000, const float displacement_x = 0.0f,
        const std::optional<std::array<float, 2u>> contact_query = {},
        const std::span<const std::array<float, 2u>> wave_queries = {}, const std::span<const u32> wave_enabled = {},
        const bool inspect_codec = false, const bool inspect_bootstrap = false) {
        auto &device = fixture.m_rhi->getDevice();
        const bool wave = not wave_queries.empty();
        auto &pipeline = inspect_bootstrap ? fixture.m_bootstrap_pipeline
                         : inspect_codec   ? fixture.m_codec_pipeline
                         : wave            ? fixture.m_wave_compare_pipeline
                         : contact_query   ? fixture.m_compare_pipeline
                                           : fixture.m_contact_pipeline;
        if (not pipeline) {
            shader::SharedModule module;
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "pbmpm" / "contact_checks.slang", "pbmpm_contact_checks",
                module.writeRef()));
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(
                inspect_bootstrap ? "compareBootstrapChecks"
                : inspect_codec   ? "inspectCompactTerrain"
                : wave            ? "compareWaveTerrainSearch"
                : contact_query   ? "compareTerrainSearch"
                                  : "inspectContact",
                entry.writeRef())));

            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc program_desc{};
            program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_desc.slangGlobalScope = module.get();
            program_desc.slangEntryPoints = entries;
            program_desc.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            rhi::ComPtr<rhi::IShaderProgram> program;
            PPR_TEST_ASSERT(not make_error_code(
                device.createShaderProgram(program_desc, program.writeRef(), diagnostics.writeRef())));

            rhi::ComputePipelineDesc pipeline_desc{};
            pipeline_desc.program = program;
            PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_desc, pipeline.writeRef())));
        }

        const auto buffer = [&](const u64 bytes, const u32 stride, const void *const data) {
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.elementSize = stride;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            rhi::ComPtr<rhi::IBuffer> result;
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, data, result.writeRef())));
            return result;
        };

        constexpr u32 node_base = (7u * 32u + 6u) * 4u;
        std::array<i32, 32u * 32u * 4u> grid{};
        grid[node_base] = static_cast<i32>(displacement_x * 10'000'000.0f);
        grid[node_base + 1u] = static_cast<i32>(displacement_y * 10'000'000.0f);
        grid[node_base + 2u] = encoded_mass;
        const std::array<u32, 6u> zero_errors{};
        const u32 empty_mask{};
        const GpuStaticShape empty_shape{};
        const GpuBodyBoundary empty_body{};
        auto grid_buffer = buffer(sizeof(grid), sizeof(i32), grid.data());
        const Array<i32> zero_node(inspect_bootstrap ? 13u : inspect_codec ? 64u : wave ? 64u * 13u + 12u : 12u, 0);
        auto result_buffer = buffer(zero_node.size() * sizeof(i32), sizeof(i32), zero_node.data());
        auto errors_buffer = buffer(sizeof(zero_errors), sizeof(u32), zero_errors.data());
        const auto packed = packTerrain(terrain);
        const auto tables = packContourTables(math::contourPoints(), math::contourCases());
        PPR_TEST_ASSERT(packed and tables);
        auto masks_buffer = buffer(
            std::max<u64>(sizeof(u32), packed->m_masks.size() * sizeof(u32)), sizeof(u32),
            packed->m_masks.empty() ? &empty_mask : packed->m_masks.data());
        auto tiles_buffer = buffer(
            std::max<u64>(sizeof(u32), packed->m_boundary_tiles.size() * sizeof(u32)), sizeof(u32),
            packed->m_boundary_tiles.empty() ? &empty_mask : packed->m_boundary_tiles.data());
        auto points_buffer = buffer(sizeof(u32), sizeof(u32), &tables->m_points);
        auto cases_buffer = buffer(sizeof(tables->m_cases), sizeof(PackedContourCase), tables->m_cases.data());
        auto reference_masks = buffer(
            std::max<u64>(sizeof(u32), terrain.m_masks.size_bytes()), sizeof(u32),
            terrain.m_masks.empty() ? &empty_mask : terrain.m_masks.data());
        auto reference_points = buffer(
            8u * sizeof(math::ContourPoint), sizeof(math::ContourPoint), math::contourPoints().data());
        auto reference_cases = buffer(
            16u * sizeof(math::ContourCase), sizeof(math::ContourCase), math::contourCases().data());
        auto bodies_buffer = buffer(
            std::max<u64>(sizeof(GpuBodyBoundary), bodies.size_bytes()), sizeof(GpuBodyBoundary),
            bodies.empty() ? &empty_body : bodies.data());
        auto shapes_buffer = buffer(
            std::max<u64>(sizeof(GpuStaticShape), shapes.size_bytes()), sizeof(GpuStaticShape),
            shapes.empty() ? &empty_shape : shapes.data());

        rhi::ComPtr<rhi::IBuffer> wave_points_buffer;
        rhi::ComPtr<rhi::IBuffer> wave_enabled_buffer;
        if (wave) {
            PPR_TEST_ASSERT(wave_queries.size() == 64u and wave_enabled.size() == 64u);
            wave_points_buffer = buffer(wave_queries.size_bytes(), 2u * sizeof(float), wave_queries.data());
            wave_enabled_buffer = buffer(wave_enabled.size_bytes(), sizeof(u32), wave_enabled.data());
        }

        GpuParameters parameters{};
        parameters.m_grid_width = 32u;
        parameters.m_grid_height = 32u;
        parameters.m_delta_time = 0.125f;
        parameters.m_shape_count = static_cast<u32>(shapes.size());
        GpuEnvironment environment{};
        environment.m_has_terrain = terrain.m_masks.empty() ? 0u : 1u;
        environment.m_terrain_width = terrain.m_width;
        environment.m_terrain_height = terrain.m_height;
        environment.m_terrain_origin_x = terrain.m_origin_x;
        environment.m_terrain_origin_y = terrain.m_origin_y;
        environment.m_body_count = static_cast<u32>(bodies.size());
        environment.m_reserved = std::ranges::any_of(
                                     packed->m_boundary_tiles, [](const u32 word) { return word != 0u; })
                                     ? 1u
                                     : 0u;

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        for (auto *const resource:
            {masks_buffer.get(), tiles_buffer.get(), points_buffer.get(), cases_buffer.get(), bodies_buffer.get(),
                shapes_buffer.get(), reference_masks.get(), reference_points.get(), reference_cases.get()}) {
            encoder->setBufferState(resource, rhi::ResourceState::ShaderResource);
        }

        encoder->setBufferState(grid_buffer, rhi::ResourceState::ShaderResource);
        if (wave) {
            encoder->setBufferState(wave_points_buffer, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(wave_enabled_buffer, rhi::ResourceState::ShaderResource);
        }

        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        rhi::IShaderObject *const root = pass->bindPipeline(pipeline);
        PPR_TEST_ASSERT(root != nullptr);
        rhi::ShaderCursor cursor{root};
        rhi::ShaderCursor constants;
        PPR_TEST_ASSERT(not make_error_code(cursor["g_parameters"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_environment"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&environment, sizeof(environment))));

        if (contact_query) {
            PPR_TEST_ASSERT(
                not make_error_code(cursor["g_contact_query"].setData(contact_query->data(), sizeof(*contact_query))));
        }

        const auto bind = [&](const char *const name, rhi::IBuffer *const resource) {
            PPR_TEST_ASSERT(not make_error_code(
                cursor[name].setBinding(rhi::Binding(resource, {.offset = 0u, .size = resource->getDesc().size}))));
        };

        bind("g_gridSrc", grid_buffer);
        bind("g_contact_result", result_buffer);
        bind("g_errors", errors_buffer);
        bind("g_shapes", shapes_buffer);
        bind("g_terrain_masks", masks_buffer);
        bind("g_terrain_boundary_tiles", tiles_buffer);
        bind("g_contour_points", points_buffer);
        bind("g_contour_cases", cases_buffer);
        bind("g_reference_masks", reference_masks);
        bind("g_reference_points", reference_points);
        bind("g_reference_cases", reference_cases);
        bind("g_bodies", bodies_buffer);
        if (wave) {
            bind("g_wave_query_points", wave_points_buffer);
            bind("g_wave_query_enabled", wave_enabled_buffer);
            bind("g_wave_query_results", result_buffer);
        }
        pass->dispatchCompute(1u, 1u, 1u);
        pass->end();

        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
        bool submitted = true;
        PPR_DEFER {
            if (submitted)
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        };

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        submitted = false;

        GridResult result{};
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(result_buffer, 0u, sizeof(result.m_node), result.m_node.data())));
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(errors_buffer, 0u, sizeof(result.m_errors), result.m_errors.data())));
        PPR_TEST_ASSERT(not make_error_code(
            device.readBuffer(result_buffer, 0u, sizeof(result.m_contacts), result.m_contacts.data())));
        if (wave) {
            result.m_wave_contacts.resize(zero_node.size());
            PPR_TEST_ASSERT(not make_error_code(device.readBuffer(
                result_buffer, 0u, result.m_wave_contacts.size() * sizeof(u32), result.m_wave_contacts.data())));
        }

        if (inspect_codec) {
            PPR_TEST_ASSERT(not make_error_code(
                device.readBuffer(result_buffer, 0u, sizeof(result.m_codec), result.m_codec.data())));
        }
        if (inspect_bootstrap) {
            u32 certified{};
            PPR_TEST_ASSERT(not make_error_code(
                device.readBuffer(result_buffer, 12u * sizeof(u32), sizeof(certified), &certified)));
            result.m_bootstrap_certified = certified != 0u;
        }
        return result;
    }

    GridResult gridContact(
        Fixture &fixture, const TerrainView terrain, const std::span<const GpuBodyBoundary> bodies,
        const float displacement_y, const std::span<const GpuStaticShape> shapes = {},
        const i32 encoded_mass = 10'000'000, const float displacement_x = 0.0f) {
        return contactProbe(fixture, terrain, bodies, displacement_y, shapes, encoded_mass, displacement_x);
    }

    GridResult compareTerrainSearch(Fixture &fixture, const TerrainView terrain, const std::array<float, 2u> point) {
        return contactProbe(fixture, terrain, {}, 0.0f, {}, 10'000'000, 0.0f, point);
    }

    // ------------------------------------------------------------------
    // bootstrap coverage certificate and unchanged fallback errors
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (bootstrap_grid_checks_preserve_fast_and_fallback_error_acceptance) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        const auto compare = [&](const TerrainView terrain, const std::span<const GpuBodyBoundary> bodies,
                                 const std::span<const GpuStaticShape> shapes, const bool certified, const u32 flags,
                                 const i32 mass = 10'000'000, const float dy = 0.0f) {
            const auto result = contactProbe(fixture, terrain, bodies, dy, shapes, mass, 0.0f, {}, {}, {}, false, true);
            PPR_TEST_ASSERT(result.m_bootstrap_certified == certified);
            PPR_TEST_ASSERT(result.m_contacts[0u] == flags);
            for (u32 field = 0u; field < 6u; ++field) {
                PPR_TEST_ASSERT(result.m_contacts[field] == result.m_contacts[6u + field]);
            }
        };

        // Mask 14 contains (.5,.5) and has a boundary; a missing witness
        // would incorrectly reject this interior query as flag 16.
        const std::array<u32, 1u> boundary{14u};
        const std::array<u32, 1u> solid{15u};
        const TerrainView with_boundary{boundary, 1u, 1u, 5.5f, 6.5f};
        const TerrainView without_boundary{solid, 1u, 1u, 5.5f, 6.5f};
        compare(with_boundary, {}, {}, true, 0u);
        compare(without_boundary, {}, {}, true, 16u);
        compare(without_boundary, {}, {}, true, 0u, 0);
        compare(without_boundary, {}, {}, true, 0u, 10'000'000, 2.0f);

        GpuBodyBoundary body{};
        body.m_shape.m_position_x = body.m_com_x = 6.0f;
        body.m_shape.m_position_y = body.m_com_y = 7.0f;
        body.m_shape.m_half_size_x = body.m_shape.m_half_size_y = 1.0f;
        body.m_shape.m_rotation_degrees = 45.0f;
        body.m_velocity_x = 2.0f;
        body.m_angular_velocity = 0.25f;
        compare(with_boundary, {&body, 1u}, {}, true, 0u);

        body.m_shape.m_shape = EStaticShape::circle;
        body.m_shape.m_radius = 1.0f;
        compare(with_boundary, {&body, 1u}, {}, false, 0u);
        body.m_shape.m_shape = EStaticShape::box;
        body.m_velocity_x = std::nextafter(1048576.0f, std::numeric_limits<float>::infinity());
        compare({}, {&body, 1u}, {}, false, 0u);

        const GpuStaticShape shape{};
        compare(without_boundary, {}, {&shape, 1u}, false, 16u);
    };

    PPR_UNIT_TEST (compact_terrain_gpu_decode_matches_wide_tables_masks_and_tile_bits) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        for (const u32 width: {7u, 8u, 9u, 31u, 32u, 33u, 257u}) {
            constexpr u32 height = 17u;
            Array<u32> masks(width * height);
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    masks[y * width + x] = ((x / 8u + y / 8u) % 3u) == 0u ? ((x + y) % 2u) * 15u : (x + y) % 16u;
                }
            }

            const auto result = contactProbe(fixture, {masks, width, height}, {}, 0.0f, {}, 0, 0.0f, {}, {}, {}, true);
            for (const u32 failed: result.m_codec)
                PPR_TEST_ASSERT(failed == 0u);
            PPR_TEST_ASSERT(result.m_errors[0u] == 0u);
        }
    };

    // ------------------------------------------------------------------
    // exact WGSL node conversion and strict accumulating conversion
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (node_wgsl_conversion_preserves_endpoints_and_scatter_rejection) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        auto &device = fixture.m_rhi->getDevice();
        shader::SharedModule module;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "contact_checks.slang", "pbmpm_conversion_checks",
            module.writeRef()));
        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("inspectNodeConversions", entry.writeRef())));

        slang::IComponentType *entries[]{entry.get()};
        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = module.get();
        program_desc.slangEntryPoints = entries;
        program_desc.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics;
        rhi::ComPtr<rhi::IShaderProgram> program;
        PPR_TEST_ASSERT(
            not make_error_code(device.createShaderProgram(program_desc, program.writeRef(), diagnostics.writeRef())));

        rhi::ComputePipelineDesc pipeline_desc{};
        pipeline_desc.program = program;
        rhi::ComPtr<rhi::IComputePipeline> pipeline;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_desc, pipeline.writeRef())));

        const auto convert = [&](const std::span<const u32> inputs, const u32 multiplier) {
            Array<std::array<u32, 12u>> output(inputs.size());
            const std::array<u32, 6u> empty_errors{};
            const auto buffer = [&](const u64 bytes, const void *const data) {
                rhi::BufferDesc desc{};
                desc.size = bytes;
                desc.elementSize = sizeof(u32);
                desc.usage = static_cast<rhi::BufferUsage>(
                    static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                    static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                    static_cast<u32>(rhi::BufferUsage::CopySource));
                desc.defaultState = rhi::ResourceState::UnorderedAccess;
                rhi::ComPtr<rhi::IBuffer> result;
                PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, data, result.writeRef())));
                return result;
            };

            auto input_buffer = buffer(inputs.size_bytes(), inputs.data());
            auto output_buffer = buffer(output.size() * sizeof(output[0]), output.data());
            auto errors_buffer = buffer(sizeof(empty_errors), empty_errors.data());
            GpuParameters parameters = fixture.m_solver.profile();
            parameters.m_fixed_point_multiplier = multiplier;
            const u32 count = static_cast<u32>(inputs.size());

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            encoder->setBufferState(input_buffer, rhi::ResourceState::ShaderResource);
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            rhi::IShaderObject *const root = pass->bindPipeline(pipeline);
            PPR_TEST_ASSERT(root != nullptr);
            rhi::ShaderCursor cursor{root};
            rhi::ShaderCursor constants;
            PPR_TEST_ASSERT(not make_error_code(cursor["g_parameters"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_count"].setData(&count, sizeof(count))));
            const auto bind = [&](const char *const name, rhi::IBuffer *const resource) {
                PPR_TEST_ASSERT(not make_error_code(
                    cursor[name].setBinding(rhi::Binding(resource, {.offset = 0u, .size = resource->getDesc().size}))));
            };

            bind("g_conversion_inputs", input_buffer);
            bind("g_conversion_results", output_buffer);
            bind("g_errors", errors_buffer);
            pass->dispatchCompute(1u, 1u, 1u);
            pass->end();

            rhi::ComPtr<rhi::ICommandBuffer> commands;
            PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
            bool submitted = true;
            PPR_DEFER {
                if (submitted)
                    PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
            };

            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
            submitted = false;
            PPR_TEST_ASSERT(not make_error_code(
                device.readBuffer(output_buffer, 0u, output.size() * sizeof(output[0]), output.data())));
            return output;
        };

        // Independent WGSL2026-10-21 section15.7.6 expectations. In particular,
        // f32's upper signed endpoint is INT_MAX-127, rather than INT_MAX.
        struct Case {
            u32 m_input_bits, m_expected_bits;
            bool m_strict_reject;
        };
        constexpr std::array<Case, 16u> cases{{
            {0x00000000u, 0u, false},
            {0x80000000u, 0u, false},
            {std::bit_cast<u32>(3.75f), 3u, false},
            {std::bit_cast<u32>(-3.75f), 0xfffffffdu, false},
            {0x4efffffeu, 0x7fffff00u, false},
            {0x4effffffu, 0x7fffff80u, false},
            {0x4f000000u, 0x7fffff80u, true},
            {0xceffffffu, 0x80000080u, false},
            {0xcf000000u, 0x80000000u, true},
            {0xcf000001u, 0x80000000u, true},
            {0x7f7fffffu, 0x7fffff80u, true},
            {0xff7fffffu, 0x80000000u, true},
            {1325912519u, 0x7fffff80u, true},
            {0x7f800000u, 0u, true},
            {0xff800000u, 0u, true},
            {0x7fc00000u, 0u, true},
        }};
        std::array<u32, cases.size()> inputs{};
        for (std::size_t index = 0u; index < cases.size(); ++index)
            inputs[index] = cases[index].m_input_bits;

        const auto results = convert(inputs, 1u);
        for (u32 index = 0u; index < cases.size(); ++index) {
            const auto &expected = cases[index];
            const auto &row = results[index];
            const bool nonfinite = not std::isfinite(std::bit_cast<float>(expected.m_input_bits));
            PPR_TEST_ASSERT(row[0] == expected.m_expected_bits);
            PPR_TEST_ASSERT(row[1] == (nonfinite ? 2u : 0u) and row[2] == (nonfinite ? 1u : 0u));
            if (nonfinite) {
                PPR_TEST_ASSERT(row[3] == index and row[4] == 0u);
                PPR_TEST_ASSERT(row[5] == 0xffff'ffffu and row[6] == 0xffff'ffffu);
            }

            PPR_TEST_ASSERT(row[7] == (expected.m_strict_reject ? 0u : expected.m_expected_bits));
            PPR_TEST_ASSERT(row[8] == (expected.m_strict_reject ? 2u : 0u));
            if (expected.m_strict_reject) {
                PPR_TEST_ASSERT(row[9] == 1u and row[10] == 0xffff'ffffu and row[11] == index);
            }
        }

        // Exercise the actual observed displacement and production multiplier,
        // not only the already-scaled failure word. INT_MIN never enters budget negation.
        const std::array<u32, 1u> observed{1130618369u};
        const auto observed_result = convert(observed, 10'000'000u);
        PPR_TEST_ASSERT(observed_result[0][0] == 0x7fffff80u and observed_result[0][1] == 0u);
        PPR_TEST_ASSERT(observed_result[0][7] == 0u and observed_result[0][8] == 2u);
    };

    std::vector<u32> rectangleMasks(const u32 width, const u32 height, const u32 last_solid_row) {
        std::vector<u32> masks(static_cast<std::size_t>(width) * height);
        const auto solid = [&](const u32 x, const u32 y) {
            return x > 0u and x < width and y > 0u and y <= last_solid_row;
        };

        for (u32 y = 0u; y < height; ++y) {
            for (u32 x = 0u; x < width; ++x) {
                masks[y * width + x] = (solid(x, y) ? 1u : 0u) |
                                       (solid(x + 1u, y) ? 2u : 0u) |
                                       (solid(x + 1u, y + 1u) ? 4u : 0u) |
                                       (solid(x, y + 1u) ? 8u : 0u);
            }
        }

        return masks;
    }


    // ------------------------------------------------------------------
    // reflected storage contracts and initialized source state
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // sparse addressing and production source-reader fixtures
    // ------------------------------------------------------------------

    struct SparseHarness {
        Fixture &m_fixture;
        SparseGridStorage m_storage;
        Array<rhi::ComPtr<rhi::IShaderObject>> m_roots;
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 3u> m_pipelines;
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 3u> m_programs;
        rhi::ComPtr<rhi::IBuffer> m_hot, m_cold, m_nodes, m_results, m_dense;
        GpuParameters m_profile{};

        explicit SparseHarness(Fixture &fixture, const u32 capacity, const u32 width = 18u, const u32 height = 18u)
            : m_fixture(fixture) {
            auto &device = fixture.m_rhi->getDevice();
            PPR_TEST_ASSERT(not m_storage.initialize(
                device, *fixture.m_shaders, std::filesystem::current_path(), capacity, width, height));

            m_profile = fixture.m_solver.profile();
            m_profile.m_grid_width = width;
            m_profile.m_grid_height = height;

            shader::SharedModule module;
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders/pbmpm/sparse_checks.slang", "pbmpm_sparse_checks",
                module.writeRef()));
            constexpr std::array<const char *, 3u> entries{
                "inspectSparseNodes", "reserveSparseAcrossGroups", "seedSparseFromDense"};
            for (u32 index = 0u; index < entries.size(); ++index) {
                shader::ComPtr<slang::IEntryPoint> entry;
                PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entries[index], entry.writeRef())));
                slang::IComponentType *selected[]{entry.get()};
                rhi::ShaderProgramDesc desc{};
                desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                desc.slangGlobalScope = module.get();
                desc.slangEntryPoints = selected;
                desc.slangEntryPointCount = 1u;
                shader::Diagnose diagnostics;
                PPR_TEST_ASSERT(not make_error_code(
                    device.createShaderProgram(desc, m_programs[index].writeRef(), diagnostics.writeRef())));
                rhi::ComputePipelineDesc pipeline{};
                pipeline.program = m_programs[index];
                PPR_TEST_ASSERT(
                    not make_error_code(device.createComputePipeline(pipeline, m_pipelines[index].writeRef())));
            }
        }

        rhi::ComPtr<rhi::IBuffer> buffer(const u64 bytes, const u32 stride, const void *const data = nullptr) {
            rhi::BufferDesc desc{};
            desc.size = std::max<u64>(bytes, stride);
            desc.elementSize = stride;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource) |
                static_cast<u32>(rhi::BufferUsage::CopyDestination));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            rhi::ComPtr<rhi::IBuffer> result;
            PPR_TEST_ASSERT(
                not make_error_code(m_fixture.m_rhi->getDevice().createBuffer(desc, data, result.writeRef())));
            return result;
        }

        void rows(const std::span<const GpuParticle> particles) {
            Array<ParticleWrite> hot(std::max<std::size_t>(1u, particles.size()));
            Array<ParticleReadonly> cold(hot.size());
            for (std::size_t row = 0u; row < particles.size(); ++row) {
                hot[row] = packWrite(particles[row]);
                cold[row] = packReadonly(particles[row]);
            }

            m_hot = buffer(hot.size() * sizeof(ParticleWrite), sizeof(ParticleWrite), hot.data());
            m_cold = buffer(cold.size() * sizeof(ParticleReadonly), sizeof(ParticleReadonly), cold.data());
            m_profile.m_particle_count = static_cast<u32>(particles.size());
        }

        void run(const std23::function_ref<void(rhi::ICommandEncoder &)> encode) {
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(m_fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            encode(*encoder);

            rhi::ComPtr<rhi::ICommandBuffer> commands;
            PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
            bool submitted = true;
            PPR_DEFER {
                if (submitted)
                    PPR_TEST_ASSERT(not make_error_code(m_fixture.m_queue->waitOnHost()));
            };
            PPR_TEST_ASSERT(not make_error_code(m_fixture.m_queue->submit(commands)));
            PPR_TEST_ASSERT(not make_error_code(m_fixture.m_queue->waitOnHost()));
            submitted = false;
            PPR_TEST_ASSERT(not m_storage.settleEncoding(SparseEncodingDisposition::executed));
            m_roots.clear();
        }

        Array<u32> read(rhi::IBuffer &source) {
            Array<u32> result(source.getDesc().size / sizeof(u32));
            PPR_TEST_ASSERT(not make_error_code(
                m_fixture.m_rhi->getDevice().readBuffer(&source, 0u, source.getDesc().size, result.data())));
            return result;
        }

        void discover(rhi::ICommandEncoder &encoder) {
            PPR_TEST_ASSERT(not m_storage.discover(encoder, *m_hot, *m_cold, m_profile, m_roots));
        }

        void seedDense(rhi::ICommandEncoder &encoder, const std::span<const u32> dense) {
            m_dense = buffer(dense.size_bytes(), sizeof(u32), dense.data());
            encoder.setBufferState(m_dense, rhi::ResourceState::ShaderResource);

            rhi::ComPtr<rhi::IShaderObject> root;
            PPR_TEST_ASSERT(not make_error_code(
                m_fixture.m_rhi->getDevice().createRootShaderObject(m_programs[2u], root.writeRef())));
            rhi::ShaderCursor cursor{root}, constants;
            const auto sparse = m_storage.constants();
            PPR_TEST_ASSERT(not make_error_code(cursor["g_sparse"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&sparse, sizeof(sparse))));

            const auto bind = [&](const char *const name, rhi::IBuffer *const resource) {
                if (cursor[name].isValid())
                    PPR_TEST_ASSERT(not make_error_code(
                        cursor[name].setBinding(
                            rhi::Binding(resource, {.offset = 0u, .size = resource->getDesc().size}))));
            };

            const auto &buffers = m_storage.buffers();
            bind("g_denseSource", m_dense);
            bind("g_pageIndexA", buffers.m_page_index[0u]);
            bind("g_pageIndexB", buffers.m_page_index[1u]);
            bind("g_payloadA", buffers.m_payload[0u]);
            bind("g_payloadB", buffers.m_payload[1u]);
            bind("g_errors", buffers.m_errors);
            m_roots.push_back(root);

            auto *const pass = encoder.beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(m_pipelines[2u], root);
            pass->dispatchCompute((m_profile.m_grid_width * m_profile.m_grid_height + 63u) / 64u, 1u, 1u);
            pass->end();
            encoder.globalBarrier();
        }

        void probe(
            rhi::ICommandEncoder &encoder, const std::span<const std::array<i32, 2u>> nodes, const bool source_a,
            const u32 generation, const u32 magnitude = 0u) {
            const u32 operation = magnitude == 0u ? 0u : 1u;
            m_nodes = buffer(nodes.size_bytes(), 8u, nodes.data());
            m_results = buffer(nodes.size() * 13u * sizeof(u32), sizeof(u32));
            encoder.setBufferState(m_nodes, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(m_results, rhi::ResourceState::UnorderedAccess);
            rhi::ComPtr<rhi::IShaderObject> root;
            PPR_TEST_ASSERT(not make_error_code(
                m_fixture.m_rhi->getDevice().createRootShaderObject(m_programs[operation], root.writeRef())));
            rhi::ShaderCursor cursor{root};
            rhi::ShaderCursor constants;
            const auto sparse = m_storage.constants(source_a);
            const std::array<u32, 4u> parameters{
                static_cast<u32>(nodes.size()), source_a ? 1u : 0u, generation, magnitude};
            PPR_TEST_ASSERT(not make_error_code(cursor["g_sparse"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&sparse, sizeof(sparse))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_probe"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(parameters.data(), sizeof(parameters))));
            const auto bind = [&](const char *const name, rhi::IBuffer *const resource) {
                if (cursor[name].isValid())
                    PPR_TEST_ASSERT(not make_error_code(
                        cursor[name].setBinding(
                            rhi::Binding(resource, {.offset = 0u, .size = resource->getDesc().size}))));
            };
            const auto &buffers = m_storage.buffers();
            bind("g_probeNodes", m_nodes);
            bind("g_probeResults", m_results);
            bind("g_pageIndexA", buffers.m_page_index[0u]);
            bind("g_pageIndexB", buffers.m_page_index[1u]);
            bind("g_payloadA", buffers.m_payload[0u]);
            bind("g_payloadB", buffers.m_payload[1u]);
            bind("g_sparseBudget", buffers.m_budget);
            bind("g_errors", buffers.m_errors);
            m_roots.push_back(root);

            auto *const pass = encoder.beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(m_pipelines[operation], root);
            pass->dispatchCompute(
                operation == 0u ? (static_cast<u32>(nodes.size()) + 63u) / 64u : static_cast<u32>(nodes.size()), 1u,
                1u);
            pass->end();
            encoder.globalBarrier();
        }
    };

    PPR_UNIT_TEST (sparse_capacity_layout_and_independent_admission_bounds) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        auto limits = fixture.m_rhi->getDevice().getInfo().limits;
        const auto world = sparseCapacity(13'044u, 4103u, 4103u, limits);
        PPR_TEST_ASSERT(
            world and world->m_buckets == 13'044u and world->m_groups == 13'044u and world->m_pages == 78'264u);
        PPR_TEST_ASSERT(world->m_lookup_bytes == 7'953'552u and world->m_dynamic_bytes == 241'209'736u);
        const auto empty = sparseCapacity(0u, 18u, 18u, limits);
        PPR_TEST_ASSERT(empty and empty->m_lookup_bytes == 216u and empty->m_dynamic_bytes == 140u);
        PPR_TEST_ASSERT(not sparseCapacity(std::numeric_limits<u32>::max(), 18u, 18u, limits));
        limits.maxComputeDispatchThreadGroups[0] = 2u;
        limits.maxComputeDispatchThreadGroups[1] = 8u;
        PPR_TEST_ASSERT(sparseCapacity(73u, 18u, 18u, limits).has_value());
        limits.maxComputeDispatchThreadGroups[1] = 4u;
        PPR_TEST_ASSERT(not sparseCapacity(73u, 18u, 18u, limits));

        shader::SharedModule module;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders/pbmpm/sparse_grid.slang", "pbmpm_sparse_grid",
            module.writeRef()));
        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        const auto field = [&](const std::string_view name) {
            for (unsigned i = 0u; i < layout->getParameterCount(); ++i) {
                auto *const parameter = layout->getParameterByIndex(i);
                if (name == parameter->getName())
                    return parameter->getTypeLayout()->getElementTypeLayout();
            }
            return static_cast<slang::TypeLayoutReflection *>(nullptr);
        };
        for (const auto [name, stride]: std::array<std::pair<const char *, u32>, 5u>{
                 {{"g_bucketIndex", 8u}, {"g_pageIndexA", 8u}, {"g_pageIndexB", 8u}, {"g_activeBuckets", 16u},
                     {"g_buckets", 16u}}}) {
            auto *const type = field(name);
            PPR_TEST_ASSERT(type != nullptr and type->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == stride);
        }
        auto *const sparse = field("g_sparse");
        auto *const physics = field("g_parameters");
        PPR_TEST_ASSERT(sparse and sparse->getSize() == 64u and physics and physics->getSize() == 64u);
        PPR_TEST_ASSERT(sparse->getFieldByIndex(8u)->getOffset() == 32u);
        PPR_TEST_ASSERT(sparse->getFieldByIndex(12u)->getOffset() == 48u);
    };

    PPR_UNIT_TEST (sparse_gpu_aprons_publish_unique_flat_keys_and_group_rows) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        {
            SparseHarness harness{fixture, 73u};
            Array<GpuParticle> particles(70u, restParticle());
            for (auto &particle: particles) {
                particle.m_position_x = 6.5f;
                particle.m_position_y = 6.5f;
            }

            for (const auto position:
                std::array<std::array<float, 2u>, 3u>{{{3.5f, 3.5f}, {12.5f, 6.5f}, {12.5f, 12.5f}}}) {
                auto particle = restParticle();
                particle.m_position_x = position[0u];
                particle.m_position_y = position[1u];
                particles.push_back(particle);
            }
            harness.rows(particles);
            PPR_TEST_ASSERT(harness.m_storage.canAdmit(73u) and not harness.m_storage.canAdmit(74u));

            Array<std::array<i32, 2u>> nodes;
            std::set<u32> expected_flat, expected_pages, expected_buckets;
            for (const auto &particle: particles) {
                const i32 bx = static_cast<i32>(particle.m_position_x / 6.0f);
                const i32 by = static_cast<i32>(particle.m_position_y / 6.0f);
                expected_buckets.insert(static_cast<u32>(by * 3 + bx));
                for (i32 y = by * 6 - 1; y <= by * 6 + 6; ++y) {
                    for (i32 x = bx * 6 - 1; x <= bx * 6 + 6; ++x) {
                        nodes.push_back({x, y});
                        if (x >= 0 and y >= 0 and x <= 18 and y <= 18 and y * 18 + x < 324) {
                            const u32 flat = static_cast<u32>(y * 18 + x);
                            expected_flat.insert(flat);
                            expected_pages.insert(((flat / 18u) / 8u) * 3u + (flat % 18u) / 8u);
                        }
                    }
                }
            }
            nodes.push_back({18, 6});
            nodes.push_back({0, 7});
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 1u));
                harness.discover(encoder);
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                harness.probe(encoder, nodes, false, harness.m_storage.constants().m_generation);
            });

            const auto &buffers = harness.m_storage.buffers();
            const auto errors = harness.read(*buffers.m_errors);
            const auto counts = harness.read(*buffers.m_counters);
            const auto pages = harness.read(*buffers.m_page_keys);
            const auto rows = harness.read(*buffers.m_rows);
            const auto groups = harness.read(*buffers.m_groups);
            const auto probe = harness.read(*harness.m_results);
            PPR_TEST_ASSERT(std::ranges::all_of(errors, [](const u32 value) { return value == 0u; }));
            PPR_TEST_ASSERT(counts[0u] == expected_buckets.size() and counts[1u] == expected_pages.size());
            PPR_TEST_ASSERT(counts[2u] == 5u and counts[3u] == particles.size());
            PPR_TEST_ASSERT(std::set<u32>(pages.begin(), pages.begin() + counts[1u]) == expected_pages);

            std::set<u32> permutation(rows.begin(), rows.begin() + counts[3u]);
            PPR_TEST_ASSERT(
                permutation.size() == particles.size() and *permutation.begin() == 0u and *permutation.rbegin() == 72u);

            u32 group_rows{};
            for (u32 group = 0u; group < counts[2u]; ++group) {
                PPR_TEST_ASSERT(groups[group * 4u + 1u] > 0u and groups[group * 4u + 1u] <= 64u);
                for (u32 offset = 0u; offset < groups[group * 4u + 1u]; ++offset) {
                    const auto &particle = particles[rows[groups[group * 4u] + offset]];
                    PPR_TEST_ASSERT(static_cast<u32>(particle.m_position_x / 6.0f) == groups[group * 4u + 2u]);
                    PPR_TEST_ASSERT(static_cast<u32>(particle.m_position_y / 6.0f) == groups[group * 4u + 3u]);
                }

                group_rows += groups[group * 4u + 1u];
            }

            PPR_TEST_ASSERT(group_rows == 73u);

            std::map<u32, u32> logical_to_physical;
            for (u32 index = 0u; index < nodes.size(); ++index) {
                const auto x = nodes[index][0u], y = nodes[index][1u];
                const bool valid = x >= 0 and y >= 0 and x <= 18 and y <= 18 and y * 18 + x < 324;
                const auto *const row = &probe[index * 13u];
                PPR_TEST_ASSERT(row[0u] == static_cast<u32>(valid));
                PPR_TEST_ASSERT(row[7u] == std::bit_cast<u32>(x) and row[8u] == std::bit_cast<u32>(y));
                if (not valid)
                    continue;

                const u32 flat = static_cast<u32>(y * 18 + x);
                PPR_TEST_ASSERT(row[1u] == flat and row[2u] == ((flat / 18u) / 8u) * 3u + (flat % 18u) / 8u);
                PPR_TEST_ASSERT(row[3u] == ((flat / 18u) % 8u) * 8u + (flat % 18u) % 8u);
                PPR_TEST_ASSERT(row[9u] == 1u and row[5u] == 0u and row[6u] == 0u);
                PPR_TEST_ASSERT(row[10u] == 0u and row[11u] == 0u and row[12u] == 0u);
                const auto [found, inserted] = logical_to_physical.emplace(flat, row[4u]);
                PPR_TEST_ASSERT(inserted or found->second == row[4u]);
            }

            PPR_TEST_ASSERT(logical_to_physical.size() == expected_flat.size());

            std::set<u32> physical;
            for (const auto &[key, address]: logical_to_physical)
                physical.insert(address);

            PPR_TEST_ASSERT(physical.size() == logical_to_physical.size());
            PPR_TEST_ASSERT(probe[(nodes.size() - 2u) * 13u + 4u] == probe[(nodes.size() - 1u) * 13u + 4u]);

            // Compare every source word against an independent dense flat array.
            // One corner bucket owns only page(0,0); all other pages are absent.
            std::array<u32, 18u * 18u * 4u> dense{};
            for (u32 flat = 0u; flat < 18u * 18u; ++flat) {
                for (u32 field = 0u; field < 4u; ++field) {
                    const i32 magnitude = static_cast<i32>(flat * 4u + field + 1u);
                    dense[flat * 4u + field] = std::bit_cast<u32>(field % 2u == 0u ? magnitude : -magnitude);
                }
            }

            std::vector<std::array<i32, 2u>> dense_nodes;
            for (i32 y = -1; y <= 18; ++y) {
                for (i32 x = -1; x <= 18; ++x)
                    dense_nodes.push_back({x, y});
            }

            auto corner = restParticle();
            corner.m_position_x = 3.5f;
            corner.m_position_y = 3.5f;
            harness.rows({&corner, 1u});
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 1u));
                harness.discover(encoder);
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                harness.seedDense(encoder, dense);
                harness.probe(encoder, dense_nodes, false, harness.m_storage.constants().m_generation);
            });

            const auto source_words = harness.read(*harness.m_results);
            constexpr std::array<u32, 4u> source_offsets{5u, 10u, 11u, 12u};
            for (u32 index = 0u; index < dense_nodes.size(); ++index) {
                const auto [x, y] = dense_nodes[index];
                const i32 flat = y * 18 + x;
                const bool valid = x >= 0 and y >= 0 and x <= 18 and y <= 18 and flat < 324;
                const bool represented = valid and flat % 18 < 8 and flat / 18 < 8;
                for (u32 field = 0u; field < 4u; ++field) {
                    const u32 expected = represented ? dense[static_cast<u32>(flat) * 4u + field] : 0u;
                    PPR_TEST_ASSERT(source_words[index * 13u + source_offsets[field]] == expected);
                }
            }
        }

        // Exact f32 neighbours of six-cell boundaries, including the failed-node witness.
        // Expected bucket coordinates derive from the boundary and side, not float division.
        {
            Array<GpuParticle> particles;
            Array<std::array<u32, 2u>> expected;

            for (const u32 boundary: std::array<u32, 4u>{6u, 12u, 1668u, 4092u}) {
                const u32 bits = std::bit_cast<u32>(static_cast<float>(boundary));

                for (u32 x_side = 0u; x_side < 3u; ++x_side) {
                    for (u32 y_side = 0u; y_side < 3u; ++y_side) {
                        auto particle = restParticle();
                        particle.m_position_x = std::bit_cast<float>(bits + x_side - 1u);
                        particle.m_position_y = std::bit_cast<float>(bits + y_side - 1u);
                        particles.push_back(particle);
                        expected.push_back(
                            {boundary / 6u - (x_side == 0u ? 1u : 0u), boundary / 6u - (y_side == 0u ? 1u : 0u)});
                    }
                }
            }

            auto witness = restParticle();
            witness.m_position_x = 4029.5f;
            witness.m_position_y = std::bit_cast<float>(std::bit_cast<u32>(1668.0f) - 1u);
            particles.push_back(witness);
            expected.push_back({671u, 277u});

            SparseHarness harness{fixture, static_cast<u32>(particles.size()), 4103u, 4103u};
            harness.rows(particles);
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 1u));
                harness.discover(encoder);
            });

            const auto &buffers = harness.m_storage.buffers();
            const auto errors = harness.read(*buffers.m_errors);
            const auto counts = harness.read(*buffers.m_counters);
            const auto groups = harness.read(*buffers.m_groups);
            const auto rows = harness.read(*buffers.m_rows);
            PPR_TEST_ASSERT(errors[0u] == 0u and counts[3u] == particles.size());

            std::set<u32> seen;
            for (u32 group = 0u; group < counts[2u]; ++group) {
                const u32 start = groups[group * 4u], count = groups[group * 4u + 1u];
                const u32 bx = groups[group * 4u + 2u], by = groups[group * 4u + 3u];
                PPR_TEST_ASSERT(count > 0u and count <= 64u and start + count <= rows.size());

                for (u32 offset = 0u; offset < count; ++offset) {
                    const u32 row = rows[start + offset];
                    PPR_TEST_ASSERT(row < particles.size() and seen.insert(row).second);
                    PPR_TEST_ASSERT(bx == expected[row][0u] and by == expected[row][1u]);

                    const auto &particle = particles[row];
                    const i32 base_x = static_cast<i32>(std::floor(particle.m_position_x)) - 1;
                    const i32 base_y = static_cast<i32>(std::floor(particle.m_position_y)) - 1;

                    for (i32 y = 0; y < 3; ++y) {
                        for (i32 x = 0; x < 3; ++x) {
                            const i32 local_x = base_x + x - (static_cast<i32>(bx) * 6 - 1);
                            const i32 local_y = base_y + y - (static_cast<i32>(by) * 6 - 1);
                            PPR_TEST_ASSERT(local_x >= 0 and local_x < 8 and local_y >= 0 and local_y < 8);
                        }
                    }
                }
            }

            PPR_TEST_ASSERT(seen.size() == particles.size());
        }
    };

    PPR_UNIT_TEST (sparse_generations_poison_capacity_and_cross_group_budget_are_transactional) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        {
            SparseHarness harness{fixture, 4u};
            {
                rhi::ComPtr<rhi::ICommandEncoder> abandoned;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(abandoned.writeRef())));
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(*abandoned, 1u));
                PPR_TEST_ASSERT(harness.m_storage.beginTick(*abandoned, 1u) == std::errc::operation_in_progress);
                abandoned.setNull();
                PPR_TEST_ASSERT(not harness.m_storage.settleEncoding(SparseEncodingDisposition::not_submitted));
            }

            auto particle = restParticle();
            particle.m_position_x = 12.5f;
            particle.m_position_y = 6.5f;
            harness.rows({&particle, 1u});
            const std::array<std::array<i32, 2u>, 2u> aliases{{{18, 6}, {0, 7}}};
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 1u));
                harness.discover(encoder);
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                harness.probe(encoder, aliases, false, harness.m_storage.constants().m_generation, 0x40000000u);
            });

            const auto &buffers = harness.m_storage.buffers();
            const auto reservations = harness.read(*harness.m_results);
            const auto errors = harness.read(*buffers.m_errors);
            const auto budgets = harness.read(*buffers.m_budget);
            const auto payload = harness.read(*buffers.m_payload[1u]);
            PPR_TEST_ASSERT(reservations[0u] == reservations[13u]);
            PPR_TEST_ASSERT(reservations[1u] + reservations[14u] == 1u);
            PPR_TEST_ASSERT(errors[0u] == 4u and errors[1u] == 1u and errors[2u] == 126u and errors[3u] == 0u);
            PPR_TEST_ASSERT(budgets[reservations[0u]] == 0x40000000u and payload[reservations[0u]] == 0x40000000u);

            const auto poison = [&](rhi::ICommandEncoder &encoder, rhi::IBuffer &buffer, const u32 value) {
                Array<u32> words(buffer.getDesc().size / sizeof(u32), value);
                encoder.setBufferState(&buffer, rhi::ResourceState::CopyDestination);
                PPR_TEST_ASSERT(
                    not make_error_code(encoder.uploadBufferData(&buffer, 0u, buffer.getDesc().size, words.data())));
                encoder.setBufferState(&buffer, rhi::ResourceState::UnorderedAccess);
                encoder.globalBarrier();
            };

            const std::array<std::array<i32, 2u>, 2u> witnesses{{{3, 3}, {17, 17}}};
            rhi::ComPtr<rhi::IBuffer> previous_source;
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 2u));
                harness.discover(encoder);
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, true, harness.m_roots));
                poison(encoder, *buffers.m_payload[0u], 0x13579bdfu);

                particle.m_position_x = 3.5f;
                particle.m_position_y = 3.5f;
                harness.rows({&particle, 1u});
                harness.discover(encoder);
                const auto old_generation = harness.m_storage.constants().m_source_generation;
                harness.probe(encoder, witnesses, true, old_generation);
                previous_source = harness.m_results;
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, true, harness.m_roots));
                harness.probe(encoder, witnesses, true, harness.m_storage.constants().m_source_generation);
            });
            const auto previous = harness.read(*previous_source);
            const auto current = harness.read(*harness.m_results);
            PPR_TEST_ASSERT(previous[5u] == 0x13579bdfu and previous[18u] == 0u and previous[22u] == 0u);
            PPR_TEST_ASSERT(
                previous[10u] == 0x13579bdfu and previous[11u] == 0x13579bdfu and previous[12u] == 0x13579bdfu);
            PPR_TEST_ASSERT(current[5u] == 0u and current[18u] == 0u and current[22u] == 0u);
            PPR_TEST_ASSERT(current[10u] == 0u and current[11u] == 0u and current[12u] == 0u);
            const auto clean_budget = harness.read(*buffers.m_budget);
            PPR_TEST_ASSERT(clean_budget[current[4u]] == 0u);

            previous_source.setNull();
            particle.m_enabled = 0.0f;
            harness.rows({&particle, 1u});
            harness.m_storage.forceNextGenerationForTest(std::numeric_limits<u32>::max());
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 2u));
                harness.discover(encoder);
                PPR_TEST_ASSERT(harness.m_storage.constants().m_generation == 1u);
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, true, harness.m_roots));
                harness.probe(encoder, witnesses, true, 1u);
            });
            PPR_TEST_ASSERT(
                std::ranges::all_of(harness.read(*buffers.m_counters), [](const u32 value) { return value == 0u; }));
            const auto after_rollover = harness.read(*harness.m_results);
            PPR_TEST_ASSERT(after_rollover[9u] == 0u and after_rollover[22u] == 0u);

            particle.m_enabled = 1.0f;
            harness.rows({&particle, 1u});
            const auto capacity = harness.m_storage.capacity();
            harness.m_storage.reduceCapacitiesForTest(1u, 1u, 1u);
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 1u));
                harness.discover(encoder);
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
            });
            const auto exact_arguments = harness.read(*buffers.m_arguments);
            PPR_TEST_ASSERT(exact_arguments[0u] == 1u and exact_arguments[4u] == 1u and exact_arguments[8u] == 1u);
            PPR_TEST_ASSERT(harness.read(*buffers.m_errors)[0u] == 0u);

            const auto admitted_hot = harness.read(*harness.m_hot);
            const auto admitted_cold = harness.read(*harness.m_cold);

            for (const auto reduced: std::array<std::array<u32, 3u>, 3u>{
                     {{0u, capacity.m_pages, capacity.m_groups}, {capacity.m_buckets, 0u, capacity.m_groups},
                         {capacity.m_buckets, capacity.m_pages, 0u}}}) {
                harness.m_storage.reduceCapacitiesForTest(reduced[0u], reduced[1u], reduced[2u]);
                harness.run([&](rhi::ICommandEncoder &encoder) {
                    PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 1u));
                    poison(encoder, *buffers.m_rows, 0xcdcdcdcdu);
                    poison(encoder, *buffers.m_payload[1u], 0xcdcdcdcdu);
                    harness.discover(encoder);
                    PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                });
                const auto rejected = harness.read(*buffers.m_errors);
                const auto arguments = harness.read(*buffers.m_arguments);
                PPR_TEST_ASSERT(rejected[0u] == 128u and rejected[1u] == 1u);
                PPR_TEST_ASSERT(arguments[0u] == 0u and arguments[4u] == 0u and arguments[8u] == 0u);
                PPR_TEST_ASSERT(std::ranges::all_of(harness.read(*buffers.m_rows), [](const u32 value) {
                    return value == 0xcdcdcdcdu;
                }));
                PPR_TEST_ASSERT(std::ranges::all_of(harness.read(*buffers.m_payload[1u]), [](const u32 value) {
                    return value == 0xcdcdcdcdu;
                }));

                PPR_TEST_ASSERT(std::ranges::equal(harness.read(*harness.m_hot), admitted_hot));
                PPR_TEST_ASSERT(std::ranges::equal(harness.read(*harness.m_cold), admitted_cold));
            }

            SparseHarness empty{fixture, 0u};
            empty.rows({});
            empty.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not empty.m_storage.beginTick(encoder, 1u));
                empty.discover(encoder);
                PPR_TEST_ASSERT(not empty.m_storage.initializeDestination(encoder, false, empty.m_roots));
                PPR_TEST_ASSERT(not empty.m_storage.initializeDestination(encoder, true, empty.m_roots));
            });
            const auto &dummy = empty.m_storage.buffers();
            PPR_TEST_ASSERT(dummy.m_payload[0u]->getDesc().size == 4u and dummy.m_budget->getDesc().size == 4u);
            PPR_TEST_ASSERT(dummy.m_errors->getDesc().size == 24u and dummy.m_arguments->getDesc().size == 48u);
            PPR_TEST_ASSERT(
                std::ranges::all_of(empty.read(*dummy.m_errors), [](const u32 value) { return value == 0u; }));
            PPR_TEST_ASSERT(
                std::ranges::all_of(empty.read(*dummy.m_counters), [](const u32 value) { return value == 0u; }));
            const auto arguments = empty.read(*dummy.m_arguments);
            PPR_TEST_ASSERT(arguments[0u] == 0u and arguments[4u] == 0u and arguments[8u] == 0u);
        }
    };

    // ------------------------------------------------------------------
    // opt-in diagnostic timing
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (deterministic_1024_sparse_dense_scene) {
        const auto configured = config::getenvLookup("PPR_TEST_DENSE1024_ROOT");
        if (not configured)
            return;

        std::error_code path_error;
        const auto root = std::filesystem::canonical(*configured, path_error);
        PPR_TEST_ASSERT(not path_error);
        std::filesystem::path output;
        for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
            const auto path = root / ("scene-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path, path_error)) {
                output = path;
                break;
            }

            PPR_TEST_ASSERT(not path_error);
        }

        PPR_TEST_ASSERT(not output.empty());
        const auto configured_validation = config::getenvLookup("PPR_TEST_DENSE1024_VALIDATE");
        const bool validation = configured_validation and *configured_validation == "1";
        std::ofstream timing_log{output / "timing.log", std::ios::binary};
        PPR_TEST_ASSERT(timing_log.is_open());
        const auto timing_writer = [&](const Log::Entry &entry) {
            if (not entry.m_message.empty()) {
                std::println(timing_log, "{} {}", entry.m_message, entry.m_params);
            }
        };

        PPR_TEST_ASSERT(not Log::flush(true));
        const auto previous_level = Log::setMinimumVerboseLevel(Log::ELevel::emphasis);
        const auto previous_writer = Log::setWriterPolicy(timing_writer);
        PPR_DEFER {
            const auto flushed = Log::flush(true);
            Log::setWriterPolicy(previous_writer);
            Log::setMinimumVerboseLevel(previous_level);
            timing_log.flush();
            PPR_TEST_ASSERT(not flushed and not timing_log.fail());
        };

        constexpr u32 edge = 1024u;
        constexpr u32 particle_count = 13044u;
        constexpr u32 ticks = 711u;
        static_assert(sizeof(GpuParticle) == 88u);
        const auto &clock = ITimerClock::steady();
        Fixture fixture;
        PPR_DEFER {
            PPR_TEST_ASSERT(not fixture.m_solver.shutdown());
            if (fixture.m_queue) {
                const hal::ProfileScope queue_wait{"MatterGpu.Scene.QueueRetirement", hal::profile_blocking_category};
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
            }

            fixture.shutdown();
        };
        fixture.initialize(true, edge, edge, validation, &clock);

        Array<GpuParticle> initial(particle_count);
        for (u32 row = 0u; row < particle_count; ++row) {
            initial[row] = restParticle();
            initial[row].m_position_x = 256.5f + static_cast<float>(row % 128u);
            initial[row].m_position_y = 160.5f + static_cast<float>(row / 128u);
        }

        const auto masks = rectangleMasks(edge, edge, 128u);
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            initial, {}, fixture.m_solver.accounting().m_epoch, {masks, edge, edge, 0.0f, 0.0f}));
        fixture.m_solver.publishReplacement();
        const auto profile = fixture.m_solver.profile();

        const auto write = [&](const std::filesystem::path &name, const void *const data, const u64 bytes) {
            std::ofstream stream{output / name, std::ios::binary};
            PPR_TEST_ASSERT(stream.is_open());
            stream.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
            stream.close();
            PPR_TEST_ASSERT(not stream.fail());
        };

        write("profile.bin", &profile, sizeof(profile));
        write("terrain.bin", masks.data(), masks.size() * sizeof(u32));
        write("initial.bin", initial.data(), initial.size() * sizeof(GpuParticle));
        std::ofstream manifest{output / "manifest.txt", std::ios::binary};
        std::ofstream status{output / "status.csv", std::ios::binary};
        PPR_TEST_ASSERT(manifest.is_open() and status.is_open());
        std::println(
            manifest,
            "scene=default-liquid-block-floor\ngrid_width={}\ngrid_height={}\nparticles={}\nrow_bytes={}\nrow_id=ordered_index\nsubsteps=8\niterations=2\ndelta_time={}\ncompleted_ticks={}\ncore_validation={}\ngpu_validation={}\nterrain_width={}\nterrain_height={}\nterrain_origin_x=0\nterrain_origin_y=0\nshapes=0\nbodies=0",
            edge, edge, particle_count, sizeof(GpuParticle), profile.m_delta_time, ticks, validation, validation, edge,
            edge);
        std::println(status, "tick,device_epoch,error,flags,node,field,particle,body");

        const auto capture = [&](const u32 completed_ticks) {
            const auto rows = fixture.snapshot();
            PPR_TEST_ASSERT(rows.size() == initial.size());
            for (u32 row = 0u; row < rows.size(); ++row) {
                const auto actual = std::bit_cast<std::array<u32, 22u>>(rows[row]);
                const auto admitted = std::bit_cast<std::array<u32, 22u>>(initial[row]);
                for (const float field: std::bit_cast<std::array<float, 22u>>(rows[row])) {
                    PPR_TEST_ASSERT(std::isfinite(field));
                }

                for (const u32 field: {13u, 14u, 15u, 18u, 19u, 20u, 21u}) {
                    PPR_TEST_ASSERT(actual[field] == admitted[field]);
                }

                if (completed_ticks == 0u)
                    PPR_TEST_ASSERT(actual == admitted);
            }

            write(
                "tick-" + std::to_string(completed_ticks) + ".particles.bin", rows.data(),
                rows.size() * sizeof(GpuParticle));
            std::println(manifest, "checkpoint={},{}", completed_ticks, fixture.m_solver.accounting().m_epoch);
            manifest.flush();
            PPR_TEST_ASSERT(not manifest.fail());
        };

        capture(0u);
        for (u32 tick = 1u; tick <= ticks; ++tick) {
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            const auto submitted = fixture.m_solver.submitResidentTick(epoch, 8u, 2u);
            if (submitted) {
                std::println(
                    stderr, "[dense1024] submit failed tick={} epoch={} error={}", tick, epoch, submitted.value());
            }

            PPR_TEST_ASSERT(not submitted);
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            const auto complete = fixture.m_solver.pollTick();
            const auto failure = fixture.m_solver.failure();
            std::println(
                status, "{},{},{},{},{},{},{},{}", tick, epoch, complete ? 0 : complete.error().value(),
                failure.m_flags, failure.m_node, failure.m_field, failure.m_particle, failure.m_body);
            status.flush();
            PPR_TEST_ASSERT(not status.fail());
            PPR_TEST_ASSERT(complete and *complete);
            PPR_TEST_ASSERT(not failure.m_cause and failure.m_flags == 0u);

            fixture.m_solver.publishTick();
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
            if (tick == 80u or tick == ticks)
                capture(tick);
        }

        const auto timing = fixture.m_solver.inspectTimingForTest();
        PPR_TEST_ASSERT(not timing.m_error and timing.m_completed_samples == ticks);
        std::println(manifest, "device_bytes={}", fixture.m_solver.accounting().m_device_bytes);
        manifest.close();
        status.close();
        PPR_TEST_ASSERT(not manifest.fail() and not status.fail());
        std::println(
            stderr, "[dense1024] completed_ticks={} particles={} validation={}", ticks, particle_count, validation);
    };

    PPR_UNIT_TEST (opt_in_stage_timing_follows_completed_fence_and_epoch) {
        Fixture fixture;
        GpuSolver observed;
        PPR_DEFER {
            PPR_TEST_ASSERT(not observed.shutdown());
            fixture.shutdown();
        };
        fixture.initialize(false);

        const auto disabled = fixture.m_solver.inspectTimingForTest();
        PPR_TEST_ASSERT(disabled.m_query_count == 0u and disabled.m_completed_samples == 0u);
        PPR_TEST_ASSERT(not disabled.m_error);
        const auto &clock = ITimerClock::steady();
        PPR_TEST_ASSERT(not observed.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            fixture.m_solver.profile(), &clock));
        const auto created = observed.inspectTimingForTest();
        PPR_TEST_ASSERT(created.m_query_count == 44u and created.m_completed_samples == 0u);
        PPR_TEST_ASSERT(not created.m_error);

        const auto particle = restParticle();
        fixture.replace({&particle, 1u});
        PPR_TEST_ASSERT(not observed.prepareReplacement({&particle, 1u}, {}, observed.accounting().m_epoch));
        observed.publishReplacement();
        // Query/readback allocations are diagnostic-only; ordinary solver buffer
        // accounting and the default-null path remain identical for the same rows.
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_device_bytes == observed.accounting().m_device_bytes);
        PPR_TEST_ASSERT(
            fixture.m_solver.accounting().m_readback_capacity_bytes == observed.accounting().m_readback_capacity_bytes);

        u64 completed{}, previous_signal{}, previous_gpu{};
        for (const u32 substeps: {1u, 8u}) {
            const auto epoch = observed.accounting().m_epoch;
            PPR_TEST_ASSERT(not observed.submitResidentTick(epoch, substeps, 2u));
            PPR_TEST_ASSERT(observed.inspectTimingForTest().m_completed_samples == completed);

            PPR_TEST_ASSERT(not observed.drain());
            const auto ready = observed.pollTick();
            PPR_TEST_ASSERT(ready and *ready);
            const auto timing = observed.inspectTimingForTest();
            PPR_TEST_ASSERT(not timing.m_error and timing.m_completed_samples == ++completed);
            PPR_TEST_ASSERT(timing.m_epoch == epoch and timing.m_signal > previous_signal);
            PPR_TEST_ASSERT(timing.m_first_gpu_timestamp >= previous_gpu);
            PPR_TEST_ASSERT(timing.m_last_gpu_timestamp >= timing.m_first_gpu_timestamp);
            previous_signal = timing.m_signal;
            previous_gpu = timing.m_last_gpu_timestamp;

            observed.publishTick();
            PPR_TEST_ASSERT(observed.accounting().m_epoch == epoch + 1u);
        }

        PPR_TEST_ASSERT(fixture.m_solver.inspectTimingForTest().m_query_count == 0u);
    };

    PPR_UNIT_TEST (initialized_rest_gravity_and_publication) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        const GpuParticle particle = restParticle();
        PPR_TEST_ASSERT(particle.m_displacement_x == 0.0f and particle.m_displacement_y == 0.0f);
        PPR_TEST_ASSERT(
            particle.m_deformation_gradient.m_c0x == 1.0f and particle.m_deformation_gradient.m_c1y == 1.0f);
        PPR_TEST_ASSERT(
            particle.m_deformation_displacement.m_c0x == 0.0f and particle.m_deformation_displacement.m_c1y == 0.0f);
        PPR_TEST_ASSERT(particle.m_liquid_density == 1.0f and particle.m_log_jp == 1.0f);
        fixture.replace({&particle, 1u});
        const u64 epoch = fixture.m_solver.accounting().m_epoch;

        // One isolated sample cannot activate volume blending: sum(W^2) < 1,
        // so the reciprocal gathered volume is > 1 in every cycle.
        fixture.finishOneSubstep();
        const auto candidate = fixture.publishSnapshot(epoch);
        PPR_TEST_ASSERT(candidate.size() == 1u);
        PPR_TEST_ASSERT(
            candidate[0].m_position_x == particle.m_position_x and candidate[0].m_position_y == particle.m_position_y);
        const GpuParameters profile = fixture.m_solver.profile();
        const float gravity = -static_cast<float>(profile.m_grid_height) *
                              profile.m_gravity_strength *
                              profile.m_delta_time *
                              profile.m_delta_time;
        PPR_TEST_ASSERT(candidate[0].m_displacement_x == 0.0f);
        PPR_TEST_ASSERT(candidate[0].m_displacement_y == gravity);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
    };

    // ------------------------------------------------------------------
    // resident publication, lifetime and complete semantic snapshots
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (resident_status_and_frozen_snapshot_preserve_full_state_without_feedback) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        GpuParticle particle = restParticle();
        particle.m_mass = 1.0e-6f;
        particle.m_deformation_displacement.m_c0x = 0.01f;
        particle.m_color_r = 0.25f;
        particle.m_lambda = 0.125f;

        GpuBodyBoundary boundary{};
        boundary.m_shape.m_position_x = 6.5f;
        boundary.m_shape.m_position_y = 6.75f;
        boundary.m_shape.m_half_size_x = 2.0f;
        boundary.m_shape.m_half_size_y = 1.0f;
        boundary.m_com_x = 6.5f;
        boundary.m_com_y = 6.75f;
        boundary.m_velocity_y = -1.0f;

        fixture.replace({&particle, 1u});
        const u64 epoch = fixture.m_solver.accounting().m_epoch;
        const auto old_view = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(old_view and old_view->m_epoch == epoch and old_view->m_count == 1u);
        PPR_TEST_ASSERT(old_view->m_live_epoch->load() == epoch);

        // A valid completed working batch still requires explicit publication.
        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u, {&boundary, 1u}, 1u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto discarded = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(discarded and *discarded);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_last_readback_bytes == 6u * sizeof(u32));
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch and old_view->m_live_epoch->load() == epoch);

        PPR_TEST_ASSERT(not fixture.m_solver.discardTick());

        const auto unchanged = fixture.snapshot();
        PPR_TEST_ASSERT(unchanged.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(unchanged.data(), &particle, sizeof(GpuParticle)) == 0);

        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u, {&boundary, 1u}, 1u));
        PPR_TEST_ASSERT(fixture.m_solver.beginSnapshot(epoch) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not fixture.m_solver.drain());

        const auto ready = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(ready and *ready);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_last_readback_bytes == 6u * sizeof(u32));

        fixture.m_solver.publishTick();
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_particle_count == 1u);

        const auto resident_view = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(resident_view and resident_view->m_epoch == epoch + 1u and resident_view->m_count == 1u);
        PPR_TEST_ASSERT(resident_view->m_hot != old_view->m_hot);
        PPR_TEST_ASSERT(old_view->m_live_epoch->load() != old_view->m_epoch);
        PPR_TEST_ASSERT(resident_view->m_live_epoch->load() == resident_view->m_epoch);

        PPR_TEST_ASSERT(fixture.m_solver.beginSnapshot(epoch) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not fixture.m_solver.beginSnapshot(epoch + 1u));
        PPR_TEST_ASSERT(fixture.m_solver.submitResidentTick(epoch + 1u, 1u, 2u) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(fixture.m_solver.prepareReplacement({}, {}, epoch + 1u) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not fixture.m_solver.drain());

        auto captured = fixture.m_solver.pollSnapshot();
        PPR_TEST_ASSERT(captured and captured->has_value());
        const auto &snapshot = **captured;
        PPR_TEST_ASSERT(snapshot.m_epoch == epoch + 1u and snapshot.m_particles.size() == 1u);
        const GpuParticle captured_value = snapshot.m_particles[0u];

        for (const float field: std::bit_cast<std::array<float, 22u>>(captured_value))
            PPR_TEST_ASSERT(std::isfinite(field));

        PPR_TEST_ASSERT(captured_value.m_mass == particle.m_mass);
        PPR_TEST_ASSERT(
            captured_value.m_material == particle.m_material and captured_value.m_volume == particle.m_volume);
        PPR_TEST_ASSERT(captured_value.m_lambda == particle.m_lambda and captured_value.m_log_jp == particle.m_log_jp);
        PPR_TEST_ASSERT(
            captured_value.m_color_r == particle.m_color_r and captured_value.m_color_g == particle.m_color_g);
        PPR_TEST_ASSERT(
            captured_value.m_color_b == particle.m_color_b and captured_value.m_enabled == particle.m_enabled);
        PPR_TEST_ASSERT(
            fixture.m_solver.accounting().m_last_readback_bytes == sizeof(ParticleWrite) + sizeof(ParticleReadonly));

        PPR_TEST_ASSERT(not fixture.m_solver.pollSnapshot());

        // Cancel after submission drains the actual source read before unlocking.
        PPR_TEST_ASSERT(not fixture.m_solver.beginSnapshot(epoch + 1u));
        PPR_TEST_ASSERT(not fixture.m_solver.cancelSnapshot());
        PPR_TEST_ASSERT(not fixture.m_solver.busy());
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == snapshot.m_epoch);

        PPR_TEST_ASSERT(not fixture.m_solver.beginSnapshot(epoch + 1u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        fixture.m_solver.injectCollectionFailureForTest(std::make_error_code(std::errc::io_error));
        const auto failed = fixture.m_solver.pollSnapshot();
        PPR_TEST_ASSERT(not failed and failed.error() == std::errc::io_error);
        PPR_TEST_ASSERT(fixture.m_solver.busy());

        PPR_TEST_ASSERT(not fixture.m_solver.cancelSnapshot());
        fixture.m_solver.injectCollectionFailureForTest({});

        // An owned capture remains exact after subsequent live publication.
        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch + 1u, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());

        const auto continued = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(continued and *continued);

        fixture.m_solver.publishTick();
        PPR_TEST_ASSERT(snapshot.m_epoch == epoch + 1u);
        PPR_TEST_ASSERT(std::memcmp(&snapshot.m_particles[0u], &captured_value, sizeof(GpuParticle)) == 0);

        fixture.replace(snapshot.m_particles);

        const auto restored = fixture.snapshot();
        PPR_TEST_ASSERT(restored.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(&restored[0u], &captured_value, sizeof(GpuParticle)) == 0);

        GpuParticle excessive = particle;
        excessive.m_mass = 100'000.0f;
        fixture.replace({&excessive, 1u});
        const u64 rejected_epoch = fixture.m_solver.accounting().m_epoch;

        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(rejected_epoch, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());

        const auto rejected = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::result_out_of_range);
        PPR_TEST_ASSERT(fixture.m_solver.failure().m_flags != 0u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == rejected_epoch);

        PPR_TEST_ASSERT(not fixture.m_solver.discardTick());

        PPR_TEST_ASSERT(not fixture.m_solver.shutdown());
        PPR_TEST_ASSERT(resident_view->m_hot != nullptr);
        PPR_TEST_ASSERT(resident_view->m_live_epoch->load() == std::numeric_limits<u64>::max());
        const auto unavailable_view = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(not unavailable_view and unavailable_view.error() == std::errc::not_connected);
    };

    PPR_UNIT_TEST (resident_append_copies_the_current_prefix_and_uploads_only_new_rows) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        auto seed = restParticle();
        seed.m_color_b = 0.375f;
        seed.m_lambda = 0.125f;
        seed.m_deformation_displacement.m_c1y = 0.01f;
        fixture.replace({&seed, 1u});

        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(fixture.m_solver.accounting().m_epoch, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto tick = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(tick and *tick);

        fixture.m_solver.publishTick();
        const u64 epoch = fixture.m_solver.accounting().m_epoch;
        PPR_TEST_ASSERT(not fixture.m_solver.beginSnapshot(epoch));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto before = fixture.m_solver.pollSnapshot();
        PPR_TEST_ASSERT(before and before->has_value());
        const auto original = (**before).m_particles[0u];

        auto appended = restParticle();
        appended.m_position_x = 15.5f;
        appended.m_color_r = 0.625f;
        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend({&appended, 1u}, epoch, {}));
        PPR_TEST_ASSERT(
            fixture.m_solver.accounting().m_last_particle_upload_bytes ==
            sizeof(ParticleWrite) + sizeof(ParticleReadonly));
        PPR_TEST_ASSERT(
            fixture.m_solver.accounting().m_last_particle_copy_bytes ==
            sizeof(ParticleWrite) + sizeof(ParticleReadonly));
        PPR_TEST_ASSERT(fixture.m_solver.beginSnapshot(epoch) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(fixture.m_solver.submitResidentTick(epoch, 1u, 2u) == std::errc::device_or_resource_busy);

        // Cancellation after a submitted copy must drain its old source and
        // keep the original publication, even when no CPU mirror exists.
        PPR_TEST_ASSERT(not fixture.m_solver.cancelPreparedReplacement());
        PPR_TEST_ASSERT(not fixture.m_solver.busy());
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_particle_count == 1u);

        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend({&appended, 1u}, epoch, {}));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto prepared = fixture.m_solver.pollReplacement();
        PPR_TEST_ASSERT(prepared and *prepared);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);

        fixture.m_solver.publishReplacement();
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_particle_count == 2u);
        PPR_TEST_ASSERT(not fixture.m_solver.beginSnapshot(epoch + 1u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto captured = fixture.m_solver.pollSnapshot();
        PPR_TEST_ASSERT(captured and captured->has_value());
        const auto &rows = (**captured).m_particles;
        PPR_TEST_ASSERT(rows.size() == 2u);
        PPR_TEST_ASSERT(std::memcmp(&rows[0u], &original, sizeof(GpuParticle)) == 0);
        PPR_TEST_ASSERT(std::memcmp(&rows[1u], &appended, sizeof(GpuParticle)) == 0);

        // A terrain-only transaction carries every old row on GPU and uploads
        // no numerical suffix. Invalid new rows cannot mutate that inventory.
        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend({}, epoch + 1u, {}));
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_last_particle_upload_bytes == 0u);
        PPR_TEST_ASSERT(
            fixture.m_solver.accounting().m_last_particle_copy_bytes ==
            2u * (sizeof(ParticleWrite) + sizeof(ParticleReadonly)));
        PPR_TEST_ASSERT(not fixture.m_solver.cancelPreparedReplacement());

        appended.m_log_jp = std::numeric_limits<float>::quiet_NaN();
        PPR_TEST_ASSERT(fixture.m_solver.prepareAppend({&appended, 1u}, epoch + 1u, {}) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not fixture.m_solver.busy());
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);

        // The newly admitted rows must fit sparse scratch independently of the
        // row buffers and execute through the same resident numerical path.
        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch + 1u, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto appended_tick = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(appended_tick and *appended_tick);

        fixture.m_solver.publishTick();
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 2u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_particle_count == 2u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_last_particle_upload_bytes == 0u);
    };

    PPR_UNIT_TEST (publication_validation_checks_every_scalar_and_matches_upload_admission) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        const GpuParticle valid = restParticle();
        std::vector<GpuParticle> rows{valid};
        std::vector<u32> expected{0xffff'ffffu};

        for (u32 field = 0u; field < 22u; ++field) {
            auto scalars = std::bit_cast<std::array<float, 22u>>(valid);
            scalars[field] = std::numeric_limits<float>::quiet_NaN();
            rows.push_back(std::bit_cast<GpuParticle>(scalars));
            expected.push_back(field);
        }

        const auto add_invalid = [&](const u32 field, const float value) {
            auto scalars = std::bit_cast<std::array<float, 22u>>(valid);
            scalars[field] = value;
            rows.push_back(std::bit_cast<GpuParticle>(scalars));
            expected.push_back(field);
        };

        add_invalid(0u, 2.999f);
        add_invalid(1u, 28.001f);
        add_invalid(12u, 0.0f);
        add_invalid(13u, -1.0f);
        add_invalid(14u, 4.0f);
        add_invalid(15u, 0.0f);
        add_invalid(21u, 0.0f);

        for (std::size_t index = 1u; index < rows.size(); ++index) {
            PPR_TEST_ASSERT(
                fixture.m_solver.prepareReplacement({&rows[index], 1u}, {}, fixture.m_solver.accounting().m_epoch) ==
                std::errc::invalid_argument);
        }

        shader::SharedModule module;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "source_checks.slang", "pbmpm_source_checks",
            module.writeRef()));
        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_TEST_ASSERT(
            not make_error_code(module->findEntryPointByName("inspectParticleValidation", entry.writeRef())));
        slang::IComponentType *entries[]{entry.get()};
        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = module.get();
        program_desc.slangEntryPoints = entries;
        program_desc.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics;
        rhi::ComPtr<rhi::IShaderProgram> program;
        auto &device = fixture.m_rhi->getDevice();
        PPR_TEST_ASSERT(
            not make_error_code(device.createShaderProgram(program_desc, program.writeRef(), diagnostics.writeRef())));
        rhi::ComputePipelineDesc pipeline_desc{};
        pipeline_desc.program = program;
        rhi::ComPtr<rhi::IComputePipeline> pipeline;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_desc, pipeline.writeRef())));

        rhi::BufferDesc desc{};
        desc.size = rows.size() * sizeof(GpuParticle);
        desc.elementSize = sizeof(GpuParticle);
        desc.usage = rhi::BufferUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        rhi::ComPtr<rhi::IBuffer> input;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, rows.data(), input.writeRef())));
        desc.size = rows.size() * sizeof(u32);
        desc.elementSize = sizeof(u32);
        desc.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsage::CopySource));
        desc.defaultState = rhi::ResourceState::UnorderedAccess;
        rhi::ComPtr<rhi::IBuffer> output;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, nullptr, output.writeRef())));

        GpuParameters parameters = fixture.m_solver.profile();
        parameters.m_particle_count = static_cast<u32>(rows.size());
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        auto *const root = pass->bindPipeline(pipeline);
        PPR_TEST_ASSERT(root != nullptr);
        rhi::ShaderCursor cursor{root};
        rhi::ShaderCursor constants;
        PPR_TEST_ASSERT(not make_error_code(cursor["g_validation_parameters"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_validation_rows"].setBinding(
                rhi::Binding(input, {.offset = 0u, .size = input->getDesc().size}))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_validation_fields"].setBinding(
                rhi::Binding(output, {.offset = 0u, .size = output->getDesc().size}))));
        pass->dispatchCompute(1u, 1u, 1u);
        pass->end();

        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
        bool may_be_inflight = true;
        PPR_DEFER {
            if (may_be_inflight) {
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
            }
        };

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        may_be_inflight = false;

        std::vector<u32> actual(rows.size());
        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(output, 0u, actual.size() * sizeof(u32), actual.data())));
        PPR_TEST_ASSERT(actual == expected);
    };

    PPR_UNIT_TEST (reflected_scalar_protocol_and_source_helpers) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        shader::SharedModule module;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "solver.slang", "pbmpm_solver", module.writeRef()));
        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        const auto find = [&](const std::string_view name) {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (name == parameter->getName())
                    return parameter->getTypeLayout()->getElementTypeLayout();
            }

            return static_cast<slang::TypeLayoutReflection *>(nullptr);
        };

        auto *const hot = find("g_hot");
        auto *const cold = find("g_cold");
        auto *const shape = find("g_shapes");
        auto *const parameters = find("g_parameters");
        PPR_TEST_ASSERT(hot != nullptr and cold != nullptr and shape != nullptr and parameters != nullptr);
        PPR_TEST_ASSERT(hot->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ParticleWrite));
        PPR_TEST_ASSERT(cold->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ParticleReadonly));
        PPR_TEST_ASSERT(shape->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GpuStaticShape));
        PPR_TEST_ASSERT(parameters->getSize() == sizeof(GpuParameters));
        PPR_TEST_ASSERT(hot->getFieldByIndex(4u)->getOffset() == PPR_OFFSETOF(ParticleWrite, m_deformation_gradient));
        PPR_TEST_ASSERT(
            hot->getFieldByIndex(5u)->getOffset() == PPR_OFFSETOF(ParticleWrite, m_deformation_displacement));
        PPR_TEST_ASSERT(hot->getFieldByIndex(9u)->getOffset() == PPR_OFFSETOF(ParticleWrite, m_enabled));
        PPR_TEST_ASSERT(cold->getFieldByIndex(0u)->getOffset() == PPR_OFFSETOF(ParticleReadonly, m_mass));
        PPR_TEST_ASSERT(cold->getFieldByIndex(3u)->getOffset() == PPR_OFFSETOF(ParticleReadonly, m_color_r));

        auto *const sparse = find("g_sparse");
        auto *const page_index = find("g_pageIndexA");
        PPR_TEST_ASSERT(sparse != nullptr and page_index != nullptr);
        PPR_TEST_ASSERT(sparse->getSize() == sizeof(SparseConstants));
        PPR_TEST_ASSERT(sparse->getFieldByIndex(8u)->getOffset() == PPR_OFFSETOF(SparseConstants, m_generation));
        PPR_TEST_ASSERT(sparse->getFieldByIndex(12u)->getOffset() == PPR_OFFSETOF(SparseConstants, m_bucket_keys));
        PPR_TEST_ASSERT(page_index->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(SparseIndex));
        PPR_TEST_ASSERT(find("g_gridSrc") == nullptr and find("g_gridDst") == nullptr and find("g_budget") == nullptr);
        PPR_TEST_ASSERT(
            find("g_counts") == nullptr and find("g_insertCounts") == nullptr and find("g_starts") == nullptr);
        PPR_TEST_ASSERT(find("g_allocator") == nullptr and find("g_args") == nullptr);

        auto *const body = find("g_bodies");
        auto *const environment = find("g_environment");
        auto *const contour = find("g_contour_cases");
        auto *const points = find("g_contour_points");
        auto *const boundary_tiles = find("g_terrain_boundary_tiles");
        PPR_TEST_ASSERT(body != nullptr and environment != nullptr and contour != nullptr and points != nullptr);
        PPR_TEST_ASSERT(boundary_tiles != nullptr);
        PPR_TEST_ASSERT(boundary_tiles->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(u32));
        PPR_TEST_ASSERT(find("g_reactions") == nullptr and find("g_reaction_budget") == nullptr);
        PPR_TEST_ASSERT(body->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GpuBodyBoundary));
        PPR_TEST_ASSERT(body->getFieldByIndex(1u)->getOffset() == PPR_OFFSETOF(GpuBodyBoundary, m_com_x));
        PPR_TEST_ASSERT(body->getFieldByIndex(5u)->getOffset() == PPR_OFFSETOF(GpuBodyBoundary, m_angular_velocity));
        PPR_TEST_ASSERT(environment->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GpuEnvironment));
        PPR_TEST_ASSERT(environment->getFieldByIndex(7u)->getOffset() == PPR_OFFSETOF(GpuEnvironment, m_reserved));
        PPR_TEST_ASSERT(
            environment->getFieldByIndex(4u)->getOffset() == PPR_OFFSETOF(GpuEnvironment, m_terrain_origin_x));
        PPR_TEST_ASSERT(contour->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(PackedContourCase));
        PPR_TEST_ASSERT(points->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(u32));

        shader::SharedModule checks;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "source_checks.slang", "pbmpm_source_checks",
            checks.writeRef()));
        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_TEST_ASSERT(not make_error_code(checks->findEntryPointByName("inspectSource", entry.writeRef())));

        slang::IComponentType *entries[]{entry.get()};
        rhi::ShaderProgramDesc desc{};
        desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        desc.slangGlobalScope = checks.get();
        desc.slangEntryPoints = entries;
        desc.slangEntryPointCount = 1u;
        rhi::ComPtr<rhi::IShaderProgram> program;
        shader::Diagnose diagnostics;
        auto &device = fixture.m_rhi->getDevice();
        PPR_TEST_ASSERT(
            not make_error_code(device.createShaderProgram(desc, program.writeRef(), diagnostics.writeRef())));

        rhi::ComputePipelineDesc pipeline_desc{};
        pipeline_desc.program = program;
        rhi::ComPtr<rhi::IComputePipeline> pipeline;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_desc, pipeline.writeRef())));

        rhi::BufferDesc buffer_desc{};
        buffer_desc.size = 23u * sizeof(float);
        buffer_desc.elementSize = sizeof(float);
        buffer_desc.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsage::CopySource));
        buffer_desc.defaultState = rhi::ResourceState::UnorderedAccess;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(buffer_desc, nullptr, buffer.writeRef())));

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        rhi::ComPtr<rhi::ICommandBuffer> commands;
        bool submitted = false;
        PPR_DEFER {
            if (submitted)
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        };

        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        rhi::IShaderObject *const root = pass->bindPipeline(pipeline);
        PPR_TEST_ASSERT(root != nullptr);
        rhi::ShaderCursor cursor{root};
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_probe"].setBinding(rhi::Binding(buffer, {.offset = 0u, .size = buffer_desc.size}))));
        pass->dispatchCompute(1u, 1u, 1u);
        pass->end();
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
        submitted = true;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        submitted = false;

        std::array<float, 23u> values{};
        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(buffer, 0u, sizeof(values), values.data())));

        PPR_TEST_ASSERT(values[0] == 23.0f and values[1] == 34.0f);
        PPR_TEST_ASSERT(values[2] == -1.0f and values[3] == 0.0f and values[4] == 9.5f and values[5] == 13.0f);
        PPR_TEST_ASSERT(values[6] == 2.0f and values[7] == 1.0f);

        // This fixed nonsymmetric reconstruction gate permits 128 float unit
        // roundoffs for the source atan2/sin/cos path, not a fitted solver band.
        const float roundoff = 128.0f * std::numeric_limits<float>::epsilon();
        PPR_TEST_ASSERT(std::abs(values[8]) <= roundoff and std::abs(values[11]) <= roundoff);
        PPR_TEST_ASSERT(std::abs(values[9] - 2.0f) <= roundoff and std::abs(values[10] + 1.0f) <= roundoff);

        PPR_TEST_ASSERT(values[12] == 0.0f and values[13] == -1.0f and values[14] == 0.0f);
        PPR_TEST_ASSERT(values[15] == 0.0f and values[16] == 0.0f);
        PPR_TEST_ASSERT(std::abs(values[17]) <= roundoff and std::abs(values[18] + 1.0f) <= roundoff);
        PPR_TEST_ASSERT(values[19] == 1.0f and values[20] == -1.0f and values[21] == 0.0f and values[22] == 0.0f);

        // The actual RHI must snapshot ordinary data at each dispatch, although
        // the same pipeline/root and buffer remain bound for both dispatches.
        entry.setNull();
        PPR_TEST_ASSERT(not make_error_code(checks->findEntryPointByName("captureParameters", entry.writeRef())));
        entries[0] = entry.get();
        rhi::ComPtr<rhi::IShaderProgram> capture_program;
        PPR_TEST_ASSERT(
            not make_error_code(device.createShaderProgram(desc, capture_program.writeRef(), diagnostics.writeRef())));
        pipeline_desc.program = capture_program;
        rhi::ComPtr<rhi::IComputePipeline> capture_pipeline;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_desc, capture_pipeline.writeRef())));

        rhi::ComPtr<rhi::ICommandEncoder> capture_encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(capture_encoder.writeRef())));
        auto *const capture_pass = capture_encoder->beginComputePass();
        PPR_TEST_ASSERT(capture_pass != nullptr);
        rhi::IShaderObject *const capture_root = capture_pass->bindPipeline(capture_pipeline);
        PPR_TEST_ASSERT(capture_root != nullptr);
        rhi::ShaderCursor capture_cursor{capture_root};
        PPR_TEST_ASSERT(not make_error_code(
            capture_cursor["g_probe"].setBinding(rhi::Binding(buffer, {.offset = 0u, .size = buffer_desc.size}))));
        for (u32 slot = 0u; slot < 2u; ++slot) {
            const float value = slot == 0u ? 11.0f : 22.0f;
            PPR_TEST_ASSERT(not make_error_code(capture_cursor["g_slot"].setData(&slot, sizeof(slot))));
            PPR_TEST_ASSERT(not make_error_code(capture_cursor["g_value"].setData(&value, sizeof(value))));
            capture_pass->dispatchCompute(1u, 1u, 1u);
        }

        capture_pass->end();
        rhi::ComPtr<rhi::ICommandBuffer> capture_commands;
        PPR_TEST_ASSERT(not make_error_code(capture_encoder->finish(capture_commands.writeRef())));
        submitted = true;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(capture_commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        submitted = false;

        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(buffer, 0u, sizeof(values), values.data())));
        PPR_TEST_ASSERT(values[0] == 11.0f and values[1] == 22.0f);
    };

    // ------------------------------------------------------------------
    // checked scatter and operational rollback
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (all_four_absolute_budgets_reject_without_publication) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        for (u32 probe = 0u; probe < 6u; ++probe) {
            const u32 field = probe < 4u ? probe : 0u;
            const bool cancellation = probe >= 4u;
            const u32 count = probe == 5u ? 128u : field < 2u ? 8u : 400u;
            std::vector<GpuParticle> particles(count, restParticle());

            for (u32 row = 0u; row < particles.size(); ++row) {
                GpuParticle &particle = particles[row];
                if (field == 0u)
                    particle.m_displacement_x = probe == 5u ? 3.0f : 100.0f;
                if (field == 1u)
                    particle.m_displacement_y = 100.0f;
                if (field == 3u)
                    particle.m_mass = 0.1f;

                if (cancellation and row % 2u != 0u)
                    particle.m_displacement_x = -particle.m_displacement_x;
            }

            // Opposing rows cancel the full signed momentum, but never its budget.
            // Eight |dx|=100 rows exceed one group's central-node budget. For128
            // |dx|=3 rows, each64-row group is bounded by1,080,000,000 encoded
            // units; their2,160,000,000 total must reject at the global merge.
            fixture.replace(particles);
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            const auto before = fixture.snapshot();

            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(not complete and complete.error() == std::make_error_code(std::errc::result_out_of_range));
            const GpuFailure failure = fixture.m_solver.failure();
            PPR_TEST_ASSERT((failure.m_flags & 4u) != 0u);
            PPR_TEST_ASSERT(failure.m_node != 0xffff'ffffu and failure.m_particle < particles.size());
            PPR_TEST_ASSERT(failure.m_field == field or (field == 2u and failure.m_field == 3u));
            PPR_TEST_ASSERT(fixture.m_solver.beginSnapshot(epoch) == std::errc::device_or_resource_busy);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
            PPR_TEST_ASSERT(not fixture.m_solver.discardTick());

            const auto after = fixture.snapshot();
            PPR_TEST_ASSERT(after.size() == before.size());
            PPR_TEST_ASSERT(std::memcmp(before.data(), after.data(), before.size() * sizeof(GpuParticle)) == 0);
        }
    };

    PPR_UNIT_TEST (strict_conversion_and_collection_retry_preserve_cause) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 2048u);
        // Scatter conversion remains rejecting; finite NODE endpoint conversion
        // is covered separately against the exact WGSL source-language contract.
        {
            GpuParticle particle = restParticle();
            particle.m_displacement_x = std::numeric_limits<float>::max();
            fixture.replace({&particle, 1u});
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            const auto result = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(not result);
            PPR_TEST_ASSERT((fixture.m_solver.failure().m_flags & 2u) != 0u);
            PPR_TEST_ASSERT(fixture.m_solver.failure().m_particle == 0u);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
            PPR_TEST_ASSERT(not fixture.m_solver.discardTick());

            const auto unchanged = fixture.snapshot();
            PPR_TEST_ASSERT(unchanged.size() == 1u);
            PPR_TEST_ASSERT(std::memcmp(unchanged.data(), &particle, sizeof(GpuParticle)) == 0);
        }

        const GpuParticle particle = restParticle();
        fixture.replace({&particle, 1u});
        const u64 epoch = fixture.m_solver.accounting().m_epoch;
        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const std::error_code cause = std::make_error_code(std::errc::interrupted);
        fixture.m_solver.injectCollectionFailureForTest(cause);
        for (u32 retry = 0u; retry < 2u; ++retry) {
            const auto result = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(not result and result.error() == cause);
            PPR_TEST_ASSERT(fixture.m_solver.failure().m_cause == cause);
            PPR_TEST_ASSERT(fixture.m_solver.beginSnapshot(epoch) == std::errc::device_or_resource_busy);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
        }

        PPR_TEST_ASSERT(not fixture.m_solver.discardTick());
        fixture.m_solver.injectCollectionFailureForTest({});

        const auto unchanged = fixture.snapshot();
        PPR_TEST_ASSERT(unchanged.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(unchanged.data(), &particle, sizeof(GpuParticle)) == 0);
    };

    PPR_UNIT_TEST (prepared_replacement_accounts_peak_and_freezes_admission) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        const GpuParticle particle = restParticle();
        fixture.replace({&particle, 1u});
        const GpuAccounting before = fixture.m_solver.accounting();
        const std::array<GpuParticle, 2u> replacement{particle, particle};
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(replacement, {}, before.m_epoch));
        const GpuAccounting staged = fixture.m_solver.accounting();
        PPR_TEST_ASSERT(
            staged.m_device_bytes == before.m_device_bytes +
                                         4u * (sizeof(ParticleWrite) + sizeof(ParticleReadonly)) +
                                         sizeof(GpuStaticShape) +
                                         8u * 36u +
                                         16u * 16u +
                                         16u * 2u +
                                         4u * 12u +
                                         12u * (12u * 256u) +
                                         16u * 2u +
                                         4u * 2u +
                                         16u +
                                         48u +
                                         24u);
        PPR_TEST_ASSERT(
            staged.m_readback_capacity_bytes == before.m_readback_capacity_bytes +
                                                    2u * (sizeof(ParticleWrite) + sizeof(ParticleReadonly)) +
                                                    6u * sizeof(u32));
        PPR_TEST_ASSERT(
            fixture.m_solver.submitResidentTick(before.m_epoch) ==
            std::make_error_code(std::errc::device_or_resource_busy));

        const auto retained = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(retained and retained->m_count == 1u and retained->m_epoch == before.m_epoch);
        PPR_TEST_ASSERT(fixture.m_solver.beginSnapshot(before.m_epoch) == std::errc::device_or_resource_busy);

        fixture.m_solver.cancelReplacement();
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_device_bytes == before.m_device_bytes);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == before.m_epoch);

        const auto unchanged = fixture.snapshot();
        PPR_TEST_ASSERT(unchanged.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(unchanged.data(), &particle, sizeof(GpuParticle)) == 0);
    };

    // ------------------------------------------------------------------
    // contour contact and one-way moving boundaries
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (all_materials_preserve_inventory_and_publish_finite_state_at_k2) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        std::array<GpuParticle, 4u> particles{};
        for (u32 material = 0u; material < particles.size(); ++material) {
            particles[material] = restParticle();
            particles[material].m_position_x = 6.5f + 4.0f * static_cast<float>(material);
            particles[material].m_material = static_cast<float>(material);
            particles[material].m_color_r = 0.125f * static_cast<float>(material + 1u);
        }

        fixture.replace(particles);
        const u64 epoch = fixture.m_solver.accounting().m_epoch;
        fixture.finishOneSubstep();
        const auto result = fixture.publishSnapshot(epoch);
        PPR_TEST_ASSERT(result.size() == particles.size());
        for (u32 row = 0u; row < result.size(); ++row) {
            const auto scalars = std::bit_cast<std::array<float, 22u>>(result[row]);
            for (const float value: scalars) {
                PPR_TEST_ASSERT(std::isfinite(value));
            }

            PPR_TEST_ASSERT(result[row].m_material == particles[row].m_material);
            PPR_TEST_ASSERT(result[row].m_mass == particles[row].m_mass);
            PPR_TEST_ASSERT(result[row].m_volume == particles[row].m_volume);
            PPR_TEST_ASSERT(result[row].m_color_r == particles[row].m_color_r);
            PPR_TEST_ASSERT(result[row].m_enabled == 1.0f);
        }
    };

    PPR_UNIT_TEST (k2_only_rejects_other_counts_before_work) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        const GpuParticle particle = restParticle();
        fixture.replace({&particle, 1u});
        const GpuAccounting before = fixture.m_solver.accounting();

        for (const u32 iterations: {0u, 1u, 3u}) {
            PPR_TEST_ASSERT(
                fixture.m_solver.submitResidentTick(before.m_epoch, 1u, iterations) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_dispatches == before.m_dispatches);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == before.m_epoch);
        }

        const auto unchanged = fixture.snapshot();
        PPR_TEST_ASSERT(unchanged.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(unchanged.data(), &particle, sizeof(particle)) == 0);
    };

    PPR_UNIT_TEST (guardian_blocks_normal_and_halves_tangent_analytically) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        // Node(6,7)+(1/4,-4)=(6.25,3) crosses only y=4. The source
        // response zeros y and lerps x halfway to0: x=1/8 exactly.
        const GridResult result = gridContact(fixture, {}, {}, -4.0f, {}, 10'000'000, 0.25f);
        PPR_TEST_ASSERT(result.m_errors[0u] == 0u);
        PPR_TEST_ASSERT(result.m_node[0u] == 1'250'000 and result.m_node[1u] == 0);
    };

    PPR_UNIT_TEST (wave_boundary_search_matches_serial_for_full_mixed_groups) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        PPR_TEST_ASSERT(fixture.m_rhi->getDevice().hasFeature(rhi::Feature::WaveOps));

        std::array<std::array<float, 2u>, 64u> points{};
        std::array<u32, 64u> enabled{};
        u32 minimum_width = 128u;
        u32 maximum_width = 0u;
        const auto compare_group = [&](const TerrainView terrain, const u32 expected_flags) {
            const auto result = contactProbe(fixture, terrain, {}, 0.0f, {}, 10'000'000, 0.0f, {}, points, enabled);
            PPR_TEST_ASSERT(result.m_wave_contacts.size() == 64u * 13u + 12u);
            for (std::size_t lane = 0u; lane < 64u; ++lane) {
                const std::size_t at = lane * 13u;
                for (std::size_t field = 0u; field < 6u; ++field) {
                    PPR_TEST_ASSERT(result.m_wave_contacts[at + field] == result.m_wave_contacts[at + 6u + field]);
                }

                const u32 width = result.m_wave_contacts[at + 12u];
                PPR_TEST_ASSERT(width >= 4u and width <= 128u);
                minimum_width = std::min(minimum_width, width);
                maximum_width = std::max(maximum_width, width);
            }

            constexpr std::size_t errors_at = 64u * 13u;
            PPR_TEST_ASSERT(result.m_wave_contacts[errors_at] == expected_flags);
            for (std::size_t field = 0u; field < 6u; ++field) {
                PPR_TEST_ASSERT(
                    result.m_wave_contacts[errors_at + field] == result.m_wave_contacts[errors_at + 6u + field]);
            }

            return result;
        };

        // Masked participants remain helpers. Pending lanes mix long searches,
        // tile edges, empty/outside queries and a rejected nonfinite coordinate.
        const auto deep_masks = rectangleMasks(256u, 200u, 180u);
        const std::array<std::array<float, 2u>, 8u> mixed{
            {{132.0f, 83.5f}, {131.5f, 91.0f}, {4.25f, 3.75f}, {-1.0f, 80.0f}, {131.5f, 193.0f},
                {std::numeric_limits<float>::quiet_NaN(), 80.0f}, {258.75f, 182.75f}, {204.0f, 83.5f}}};
        for (std::size_t lane = 0u; lane < points.size(); ++lane) {
            points[lane] = mixed[lane % mixed.size()];
            enabled[lane] = lane % 5u != 0u ? 1u : 0u;
        }

        const auto deep = compare_group({deep_masks, 256u, 200u, 3.5f, 3.0f}, 0u);
        PPR_TEST_ASSERT(deep.m_wave_contacts[13u] == 1u);

        // Owners near the end of the cohort exercise uniform dynamic broadcasts
        // and exact equal-distance nearest-boundary tie selection.
        const auto tie_masks = rectangleMasks(17u, 17u, 16u);
        points.fill({12.0f, 11.5f});
        enabled.fill(0u);
        enabled[31u] = 1u;
        enabled[63u] = 1u;
        compare_group({tie_masks, 17u, 17u, 3.5f, 3.0f}, 0u);

        const auto edge_masks = rectangleMasks(19u, 25u, 17u);
        const std::array<std::array<float, 2u>, 4u> edges{
            {{11.0f, 11.5f}, {11.5f, 18.5f}, {21.75f, 19.75f}, {4.25f, 3.75f}}};
        for (std::size_t lane = 0u; lane < points.size(); ++lane)
            points[lane] = edges[lane % edges.size()];

        enabled.fill(1u);
        compare_group({edge_masks, 19u, 25u, 3.5f, 3.0f}, 0u);

        // Exactly one malformed query makes all six first-error words stable.
        const std::array<u32, 1u> solid{15u};
        points.fill({4.0f, 3.5f});
        enabled.fill(0u);
        enabled[31u] = 1u;
        compare_group({solid, 1u, 1u, 3.5f, 3.0f}, 16u);

        enabled.fill(1u);
        compare_group({}, 0u);
        std::println(
            "[terrain-wave-check] threads=64 wave_lanes_min={} wave_lanes_max={}", minimum_width, maximum_width);
    };

    PPR_UNIT_TEST (terrain_endpoint_response_and_complete_boundary_search) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        // Compare every Contact field bit-for-bit with the frozen ring search.
        // The first case crosses more than 76 empty rings. The remaining cases
        // cover tile boundaries, partial edge tiles, out-of-world ring spans,
        // and equal nearest distances whose winner depends on traversal order.
        const auto compare_search = [&](const u32 width, const u32 height, const u32 solid_top,
                                        const std::span<const std::array<float, 2u>> queries) {
            const auto reference_masks = rectangleMasks(width, height, solid_top);
            const TerrainView reference_terrain{reference_masks, width, height, 3.5f, 3.0f};
            for (const auto &local: queries) {
                const std::array<float, 2u> point{local[0] + 3.5f, local[1] + 3.0f};
                const auto result = compareTerrainSearch(fixture, reference_terrain, point);
                PPR_TEST_ASSERT(result.m_errors[0u] == 0u);
                PPR_TEST_ASSERT(result.m_contacts[0u] == 1u);
                for (std::size_t field = 0u; field < 6u; ++field) {
                    PPR_TEST_ASSERT(result.m_contacts[field] == result.m_contacts[field + 6u]);
                }
            }
        };

        const std::array<std::array<float, 2u>, 2u> long_queries{{{128.5f, 80.5f}, {128.0f, 88.0f}}};
        compare_search(256u, 200u, 180u, long_queries);

        const std::array<std::array<float, 2u>, 4u> edge_queries{
            {{7.5f, 8.5f}, {8.0f, 15.5f}, {18.25f, 16.75f}, {0.75f, 0.75f}}};
        compare_search(19u, 25u, 17u, edge_queries);

        const std::array<std::array<float, 2u>, 1u> tie_queries{{{8.5f, 8.5f}}};
        compare_search(17u, 17u, 16u, tie_queries);

        // Retain the non-power-of-two depth-six geometry as an exact GPU
        // old/new Contact comparison, without a CPU division-parity premise.
        const std::array<std::array<float, 2u>, 1u> depth_six_queries{{{9.0f, 9.5f}}};
        compare_search(24u, 24u, 15u, depth_six_queries);

        const auto masks = rectangleMasks(8u, 4u, 2u);
        const TerrainView terrain{masks, 8u, 4u, 3.0f, 5.0f};
        for (const float dy: {0.0f, -0.25f, 0.25f}) {
            const GridResult result = gridContact(fixture, terrain, {}, dy);
            PPR_TEST_ASSERT(result.m_errors[0] == 0u);
            PPR_TEST_ASSERT(result.m_node[0] == 0);
            PPR_TEST_ASSERT(result.m_node[1] == (dy > 0.0f ? 2'500'000 : 0));
        }

        // The centre of either disconnected saddle is outside both triangles.
        for (const u32 saddle: {5u, 10u}) {
            const std::array<u32, 1u> mask{saddle};
            const GridResult result = gridContact(fixture, {mask, 1u, 1u, 5.5f, 6.5f}, {}, 0.125f);
            PPR_TEST_ASSERT(result.m_errors[0] == 0u and result.m_node[1] == 1'250'000);
        }

        // An unsupported node must not query a solid mask lacking a boundary.
        // Supported admission of that same incomplete geometry reports flag16.
        const std::array<u32, 1u> solid_mask{15u};
        const TerrainView incomplete{solid_mask, 1u, 1u, 5.5f, 6.5f};
        const GridResult empty = gridContact(fixture, incomplete, {}, 0.0f, {}, 0);
        PPR_TEST_ASSERT(empty.m_errors[0] == 0u and empty.m_node[1] == 0);
        const GridResult missing_boundary = gridContact(fixture, incomplete, {}, 0.0f);
        PPR_TEST_ASSERT((missing_boundary.m_errors[0] & 16u) != 0u);

        // The unique nearest boundary is eight cells above the sample (left
        // 8.5, bottom 9, right 14.5). Its power-of-two depth makes normalization
        // exact for this dyadic geometry; no CPU/GPU division-parity assumption.
        // No grid motion exists, so p stays fixed and only retained d changes.
        const auto deep_masks = rectangleMasks(24u, 24u, 17u);
        GpuParticle particle = restParticle();
        particle.m_position_x = 12.5f;
        particle.m_position_y = 12.5f;
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            {&particle, 1u}, {}, fixture.m_solver.accounting().m_epoch, {deep_masks, 24u, 24u, 3.5f, 3.0f}));
        fixture.m_solver.publishReplacement();
        fixture.finishOneSubstep();
        const GpuParticle candidate = fixture.publishSnapshot(fixture.m_solver.accounting().m_epoch)[0];
        const auto profile = fixture.m_solver.profile();
        const float gravity = -static_cast<float>(profile.m_grid_height) *
                              profile.m_gravity_strength *
                              profile.m_delta_time *
                              profile.m_delta_time;

        PPR_TEST_ASSERT(candidate.m_position_x == 12.5f and candidate.m_position_y == 12.5f);
        PPR_TEST_ASSERT(candidate.m_displacement_x == 0.0f);
        PPR_TEST_ASSERT(candidate.m_displacement_y == gravity + 8.0f);

        // A replacement with the same dimensions moves its boundary to a new
        // tile row. Depth four retains the original power-of-two normalization
        // premise; the old table has no boundary in the new interior tile.
        const auto changed_masks = rectangleMasks(24u, 24u, 13u);
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            {&particle, 1u}, {}, fixture.m_solver.accounting().m_epoch, {changed_masks, 24u, 24u, 3.5f, 3.0f}));
        fixture.m_solver.publishReplacement();
        fixture.finishOneSubstep();

        const GpuParticle changed = fixture.publishSnapshot(fixture.m_solver.accounting().m_epoch)[0];
        PPR_TEST_ASSERT(changed.m_position_x == 12.5f and changed.m_position_y == 12.5f);
        PPR_TEST_ASSERT(changed.m_displacement_x == 0.0f);
        PPR_TEST_ASSERT(changed.m_displacement_y == gravity + 4.0f);
    };

    PPR_UNIT_TEST (moving_body_static_limit_translation_rotation_and_one_way_contact) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        GpuBodyBoundary boundary{};
        boundary.m_shape.m_position_x = 6.0f;
        boundary.m_shape.m_position_y = 7.5f;
        boundary.m_shape.m_half_size_x = 2.0f;
        boundary.m_shape.m_half_size_y = 1.0f;
        boundary.m_com_x = 6.0f;
        boundary.m_com_y = 7.5f;

        const GridResult literal = gridContact(fixture, {}, {}, 0.25f, {&boundary.m_shape, 1u});
        const GridResult stationary = gridContact(fixture, {}, {&boundary, 1u}, 0.25f);
        PPR_TEST_ASSERT(literal.m_errors[0] == 0u and stationary.m_errors[0] == 0u);
        PPR_TEST_ASSERT(literal.m_node == stationary.m_node);
        PPR_TEST_ASSERT(stationary.m_node[1] == 0);

        boundary.m_velocity_y = -1.0f;
        const GridResult translated = gridContact(fixture, {}, {&boundary, 1u}, 0.0f);
        PPR_TEST_ASSERT(translated.m_errors[0] == 0u);
        PPR_TEST_ASSERT(translated.m_node[1] == -1'250'000);

        boundary.m_velocity_y = 0.0f;
        boundary.m_com_x = 5.0f;
        boundary.m_com_y = 7.0f;
        boundary.m_angular_velocity = -1.0f;
        const GridResult rotated = gridContact(fixture, {}, {&boundary, 1u}, 0.0f);
        PPR_TEST_ASSERT(rotated.m_errors[0] == 0u and rotated.m_node[1] == -1'250'000);

        GpuBodyBoundary horizontal = boundary;
        horizontal.m_shape.m_position_x = 6.5f;
        horizontal.m_shape.m_position_y = 7.0f;
        horizontal.m_shape.m_half_size_x = 1.0f;
        horizontal.m_shape.m_half_size_y = 2.0f;
        horizontal.m_com_x = 6.0f;
        horizontal.m_com_y = 7.0f;
        horizontal.m_angular_velocity = 0.0f;
        horizontal.m_velocity_x = -1.0f;
        const GridResult left = gridContact(fixture, {}, {&horizontal, 1u}, 0.0f);
        PPR_TEST_ASSERT(left.m_errors[0] == 0u and left.m_node[0] == -1'250'000);
    };

    PPR_UNIT_TEST (body_admission_rejection_and_same_epoch_contact_publication) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        const GpuParticle particle = restParticle();
        fixture.replace({&particle, 1u});
        const u64 epoch = fixture.m_solver.accounting().m_epoch;
        GpuBodyBoundary boundary{};
        boundary.m_shape.m_position_x = 6.5f;
        boundary.m_shape.m_position_y = 6.75f;
        boundary.m_shape.m_half_size_x = 2.0f;
        boundary.m_shape.m_half_size_y = 1.0f;
        boundary.m_com_x = 6.5f;
        boundary.m_com_y = 6.75f;
        boundary.m_velocity_y = std::numeric_limits<float>::quiet_NaN();

        PPR_TEST_ASSERT(
            fixture.m_solver.submitResidentTick(epoch, 1u, 2u, {&boundary, 1u}, 1u) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not fixture.m_solver.busy());
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);

        const auto unchanged = fixture.snapshot();
        PPR_TEST_ASSERT(unchanged.size() == 1u and std::memcmp(&particle, unchanged.data(), sizeof(particle)) == 0);

        // Retry the same committed epoch with the original full body schedule.
        boundary.m_velocity_y = 0.0f;
        const std::array<GpuBodyBoundary, 8u> boundaries{
            boundary, boundary, boundary, boundary, boundary, boundary, boundary, boundary};
        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 8u, 2u, boundaries, 1u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto complete = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(complete and *complete);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_body_count == 1u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_substeps == 8u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_last_readback_bytes == 6u * sizeof(u32));
        fixture.m_solver.publishTick();
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
    };

    PPR_UNIT_TEST (predicted_body_slices_select_distinct_substep_rows) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        GpuParticle particle = restParticle();
        particle.m_mass = 1.0e-6f;
        GpuBodyBoundary colliding_boundary{};
        colliding_boundary.m_shape.m_position_x = 6.5f;
        colliding_boundary.m_shape.m_position_y = 6.75f;
        colliding_boundary.m_shape.m_half_size_x = 2.0f;
        colliding_boundary.m_shape.m_half_size_y = 1.0f;
        colliding_boundary.m_com_x = 6.5f;
        colliding_boundary.m_com_y = 6.75f;
        GpuBodyBoundary distant_boundary = colliding_boundary;
        distant_boundary.m_shape.m_position_x = 20.0f;
        distant_boundary.m_com_x = 20.0f;

        // Each nodal mass is <=1e-6, below the source 1e-5 threshold:
        // every gather is d=D=0. Retained |d|<1 also proves a zero encoded
        // denominator cannot carry nonzero encoded momentum on the next cycle.
        // Positions stay fixed, and only the LAST substep's late box contact
        // determines final retained d. Box sign normals and depth .75 are exact.
        const auto profile = fixture.m_solver.profile();
        const float gravity = -static_cast<float>(profile.m_grid_height) *
                              profile.m_gravity_strength *
                              profile.m_delta_time *
                              profile.m_delta_time;

        for (const bool near_last: {true, false}) {
            fixture.replace({&particle, 1u});
            const std::array<GpuBodyBoundary, 2u> boundaries =
                near_last ? std::array<GpuBodyBoundary, 2u>{distant_boundary, colliding_boundary}
                          : std::array<GpuBodyBoundary, 2u>{colliding_boundary, distant_boundary};

            PPR_TEST_ASSERT(
                not fixture.m_solver.submitResidentTick(fixture.m_solver.accounting().m_epoch, 2u, 2u, boundaries, 1u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);

            const GpuParticle candidate = fixture.publishSnapshot(fixture.m_solver.accounting().m_epoch)[0];
            PPR_TEST_ASSERT(
                candidate.m_position_x == particle.m_position_x and candidate.m_position_y == particle.m_position_y);
            PPR_TEST_ASSERT(candidate.m_displacement_x == 0.0f);
            PPR_TEST_ASSERT(candidate.m_displacement_y == (near_last ? gravity - 0.75f : gravity));
        }

        // A fixed bodyBase=0 would see away/away or near/near and reverses
        // both expected last-contact outcomes above, so this gate detects it.
    };

    // ------------------------------------------------------------------
    // dual-owner peak footprint preflight
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (device_peak_footprint_for_dual_grid_ownership) {
        constexpr u32 kEdge = 4103u;
        constexpr u32 kAdmitted = 13'044u;
        constexpr u64 kRowBytes = sizeof(ParticleWrite) + sizeof(ParticleReadonly);
        constexpr u64 kBucketEdge = (kEdge + 5u) / 6u;
        constexpr u64 kBucketCount = kBucketEdge * kBucketEdge;
        constexpr u64 kPageEdge = (kEdge + 7u) / 8u;
        constexpr u64 kPageCount = kPageEdge * kPageEdge;
        constexpr u64 kLookupBytes = 8u * kBucketCount + 16u * kPageCount;
        static_assert(kRowBytes == 88u and kLookupBytes == 7'953'552u);

        // Independent allocation arithmetic, including every zero-row dummy.
        // No solver accounting subtotal or sparseCapacity result is the oracle.
        constexpr u64 kFixedBytes =
            kLookupBytes + sizeof(PackedContourTables) + sizeof(u32) + sizeof(GpuBodyBoundary) + sizeof(GpuStaticShape);
        constexpr auto storageBytes = [=](const u64 count) {
            const u64 rows = std::max<u64>(1u, count);
            const u64 buckets = std::min(count, kBucketCount);
            const u64 groups = std::min(count, (count + 63u * buckets) / 64u);
            const u64 pages = std::min(6u * buckets, kPageCount);
            return 2u * rows * kRowBytes +
                   16u * std::max<u64>(1u, buckets) +
                   4u * std::max<u64>(1u, pages) +
                   12u * std::max<u64>(1u, 256u * pages) +
                   16u * std::max<u64>(1u, groups) +
                   4u * rows +
                   16u +
                   48u +
                   24u;
        };

        constexpr u64 kEmptyPeak = 2u * (kFixedBytes + storageBytes(0u));
        constexpr u64 kProjectedPeak = 2u * (kFixedBytes + storageBytes(kAdmitted));
        constexpr u64 kExpectedGrowth = 4u * (kAdmitted - 1u) * kRowBytes +
                                        2u * (16u * (kAdmitted - 1u) +
                                                 4u * (6u * kAdmitted - 1u) +
                                                 12u * (256u * 6u * kAdmitted - 1u) +
                                                 16u * (kAdmitted - 1u) +
                                                 4u * (kAdmitted - 1u));
        static_assert(kEmptyPeak == 15'908'200u);
        static_assert(kProjectedPeak == 502'918'528u);
        static_assert(kExpectedGrowth == 487'010'328u and kProjectedPeak - kEmptyPeak == kExpectedGrowth);

        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, kEdge, kEdge);

        GpuSolver second;
        PPR_TEST_ASSERT(not second.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            fixture.m_solver.profile()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not second.shutdown());
        };

        const u64 empty_bytes = fixture.m_solver.accounting().m_device_bytes + second.accounting().m_device_bytes;
        PPR_TEST_ASSERT(empty_bytes == kEmptyPeak);

        Array<GpuParticle> particles(kAdmitted);
        for (u32 row = 0u; row < kAdmitted; ++row) {
            GpuParticle particle = restParticle();
            particle.m_position_x = 3.0f + static_cast<float>(row % 4096u);
            particle.m_position_y = 3.0f + static_cast<float>((row * 7u) % 4096u);
            particles[row] = particle;
        }

        for (GpuSolver *const solver: std::array<GpuSolver *, 2u>{&fixture.m_solver, &second}) {
            PPR_TEST_ASSERT(not solver->prepareReplacement(particles, {}, solver->accounting().m_epoch));
            solver->publishReplacement();
        }

        const u64 loaded_bytes = fixture.m_solver.accounting().m_device_bytes + second.accounting().m_device_bytes;
        PPR_TEST_ASSERT(loaded_bytes == kProjectedPeak);
        PPR_TEST_ASSERT(loaded_bytes - empty_bytes == kExpectedGrowth);

        const rhi::DeviceInfo &info = fixture.m_rhi->getDevice().getInfo();
        const char *const api = info.apiName != nullptr ? info.apiName : "unknown";
        const char *const adapter = info.adapterName != nullptr ? info.adapterName : "unknown";
        std::fprintf(
            stderr, "[matter-peak] rows=%u lookup=%llu empty=%llu loaded=%llu projected=%llu api=%s adapter=%s\n",
            kAdmitted, static_cast<unsigned long long>(kLookupBytes), static_cast<unsigned long long>(empty_bytes),
            static_cast<unsigned long long>(loaded_bytes), static_cast<unsigned long long>(kProjectedPeak), api,
            adapter);
    };
}

namespace pP::tests {
    const UnitTest &matter_gpuTests() noexcept {
        static const UnitTest group = UnitTest::Named("matter_gpu") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::MatterGpu::node_wgsl_conversion_preserves_endpoints_and_scatter_rejection,
                detail::MatterGpu::bootstrap_grid_checks_preserve_fast_and_fallback_error_acceptance,
                detail::MatterGpu::sparse_capacity_layout_and_independent_admission_bounds,
                detail::MatterGpu::sparse_gpu_aprons_publish_unique_flat_keys_and_group_rows,
                detail::MatterGpu::sparse_generations_poison_capacity_and_cross_group_budget_are_transactional,
                detail::MatterGpu::opt_in_stage_timing_follows_completed_fence_and_epoch,
                detail::MatterGpu::deterministic_1024_sparse_dense_scene,
                detail::MatterGpu::initialized_rest_gravity_and_publication,
                detail::MatterGpu::all_materials_preserve_inventory_and_publish_finite_state_at_k2,
                detail::MatterGpu::k2_only_rejects_other_counts_before_work,
                detail::MatterGpu::guardian_blocks_normal_and_halves_tangent_analytically,
                detail::MatterGpu::split_device_rows_pack_and_unpack_every_semantic_field_by_name,
                detail::MatterGpu::lossless_terrain_codecs_preserve_tables_tails_and_reject_invalid_input,
                detail::MatterGpu::compact_terrain_gpu_decode_matches_wide_tables_masks_and_tile_bits,
                detail::MatterGpu::reflected_scalar_protocol_and_source_helpers,
                detail::MatterGpu::resident_status_and_frozen_snapshot_preserve_full_state_without_feedback,
                detail::MatterGpu::resident_append_copies_the_current_prefix_and_uploads_only_new_rows,
                detail::MatterGpu::publication_validation_checks_every_scalar_and_matches_upload_admission,
                detail::MatterGpu::all_four_absolute_budgets_reject_without_publication,
                detail::MatterGpu::strict_conversion_and_collection_retry_preserve_cause,
                detail::MatterGpu::prepared_replacement_accounts_peak_and_freezes_admission,
                detail::MatterGpu::terrain_endpoint_response_and_complete_boundary_search,
                detail::MatterGpu::wave_boundary_search_matches_serial_for_full_mixed_groups,
                detail::MatterGpu::moving_body_static_limit_translation_rotation_and_one_way_contact,
                detail::MatterGpu::body_admission_rejection_and_same_epoch_contact_publication,
                detail::MatterGpu::predicted_body_slices_select_distinct_substep_rows,
                detail::MatterGpu::device_peak_footprint_for_dual_grid_ownership,
            });
        };
        return group;
    }
}
