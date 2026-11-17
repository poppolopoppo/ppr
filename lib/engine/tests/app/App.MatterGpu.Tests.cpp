module;

#include "pP/Macros.h"
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
import engine.app;
import engine.image;

namespace pP::tests::detail::MatterGpu {
    using namespace pP::matter;
    PPR_DEFINE_LOG_CATEGORY(MatterPoolRender, info, none)

    // ------------------------------------------------------------------
    // explicit native headless GPU fixture
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
        rhi::ComPtr<rhi::IComputePipeline> m_transport_pipeline;
        rhi::ComPtr<rhi::IComputePipeline> m_exterior_pipeline;
        rhi::ComPtr<rhi::IComputePipeline> m_repair_pipeline;
        rhi::ComPtr<rhi::IComputePipeline> m_sand_contact_pipeline;
        GpuSolver m_solver;

        void initialize(
            const bool use_volume = true, const u32 grid_width = 32u, const u32 grid_height = 32u,
            const bool validation = true, const ITimerClock *const observation_clock = nullptr,
            const float gravity_strength = 2.5f) {
            PPR_TEST_ASSERT(not m_shaders->initialize());
            m_rhi->setCoreValidation(validation);
            m_rhi->setGpuAssistedValidation(validation);
            PPR_TEST_ASSERT(m_rhi->isCoreValidationEnabled() == validation);
            PPR_TEST_ASSERT(m_rhi->isGpuAssistedValidationEnabled() == validation);
#ifdef _WIN32
            constexpr auto backend = rhi::DeviceType::D3D12;
#else
            constexpr auto backend = rhi::DeviceType::Vulkan;
#endif

            PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
            PPR_TEST_ASSERT(m_rhi->getDevice().getDeviceType() == backend);
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            GpuParameters parameters{};
            parameters.m_grid_width = grid_width;
            parameters.m_grid_height = grid_height;
            parameters.m_gravity_strength = gravity_strength;
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
            m_sand_contact_pipeline.setNull();
            m_repair_pipeline.setNull();
            m_exterior_pipeline.setNull();
            m_transport_pipeline.setNull();
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

        void replace(const std::span<const GpuParticle> particles, const std::span<const GpuStaticShape> shapes = {}) {
            PPR_TEST_ASSERT(not m_solver.prepareReplacement(particles, shapes, m_solver.accounting().m_epoch));
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

        /// Publishes one collected tick and reads it back, without the frozen
        /// inspection probe publishSnapshot makes. That probe deliberately calls
        /// beginSnapshot while the tick is still pending, so the solver logs an
        /// expected device_or_resource_busy at every call; a leaf using this
        /// helper instead emits no such line, and every busy observation it would
        /// make is a sequencing defect rather than expected output.
        Array<GpuParticle> publishCompletedTick() {
            const u64 epoch = m_solver.accounting().m_epoch;
            // Complete but unpublished: the collected status proves pollTick ran,
            // and busy() proves nothing published or cancelled it in between.
            PPR_TEST_ASSERT(m_solver.busy());
            PPR_TEST_ASSERT(m_solver.accounting().m_last_readback_bytes == 6u * sizeof(u32));

            m_solver.publishTick();
            PPR_TEST_ASSERT(m_solver.accounting().m_epoch == epoch + 1u);
            PPR_TEST_ASSERT(not m_solver.busy());

            return snapshot();
        }

        void finishOneSubstep(const u32 iterations = kSourceIterations) {
            // A submitted batch owns the solver until it is published, so prove the
            // previous one was drained, polled and published before submitting.
            PPR_TEST_ASSERT(not m_solver.busy());
            PPR_TEST_ASSERT(not m_solver.submitResidentTick(m_solver.accounting().m_epoch, 1u, iterations));

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

    // Diagnostic timestamp pool size: the solver sizes its opt-in query pool for
    // the largest admitted schedule, kGameSubsteps * (2 * 10 + 1) + 4 marks, not for
    // the marks one particular batch uses. Both opt-in timing leaves pin this so a
    // schedule bound change shows up in one place.
    constexpr u32 kDiagnosticQueryCount = 172u;

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
        PPR_TEST_ASSERT(sizeof(cold) == 28u and sizeof(hot) == 72u and sizeof(particle) == 96u);
        PPR_TEST_ASSERT(cold.m_property_row == std::numeric_limits<u32>::max());

        const GpuParticle restored = unpack(hot, cold);
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 24u>>(restored) == std::bit_cast<std::array<u32, 24u>>(particle));

        GpuMaterialProperties property{};
        property.m_substance_id = 1u;
        PPR_TEST_ASSERT(validMaterialProperties(property));
        for (u32 field = 2u; field < 19u; ++field) {
            auto words = std::bit_cast<std::array<u32, 19u>>(property);
            words[field] = std::bit_cast<u32>(std::numeric_limits<float>::quiet_NaN());
            PPR_TEST_ASSERT(not validMaterialProperties(std::bit_cast<GpuMaterialProperties>(words)));
        }

        property.m_family = static_cast<ESourceMaterial>(6u);
        PPR_TEST_ASSERT(not validMaterialProperties(property));
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
        const bool inspect_codec = false, const bool inspect_bootstrap = false, const bool inspect_transport = false,
        const bool inspect_exterior = false, const bool inspect_sand_contact = false,
        const bool inspect_repair = false) {
        auto &device = fixture.m_rhi->getDevice();
        const bool wave = not wave_queries.empty();
        auto &pipeline = inspect_sand_contact ? fixture.m_sand_contact_pipeline
                         : inspect_repair     ? fixture.m_repair_pipeline
                         : inspect_exterior   ? fixture.m_exterior_pipeline
                         : inspect_transport  ? fixture.m_transport_pipeline
                         : inspect_bootstrap  ? fixture.m_bootstrap_pipeline
                         : inspect_codec      ? fixture.m_codec_pipeline
                         : wave               ? fixture.m_wave_compare_pipeline
                         : contact_query      ? fixture.m_compare_pipeline
                                              : fixture.m_contact_pipeline;
        if (not pipeline) {
            shader::SharedModule module;
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "pbmpm" / "contact_checks.slang", "pbmpm_contact_checks",
                module.writeRef()));
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(
                inspect_sand_contact ? "inspectSandGridContacts"
                : inspect_repair     ? "inspectTerrainPositionRepair"
                : inspect_exterior   ? "inspectTerrainExteriorEndpoint"
                : inspect_transport  ? "inspectTerrainTransportBudget"
                : inspect_bootstrap  ? "compareBootstrapChecks"
                : inspect_codec      ? "inspectCompactTerrain"
                : wave               ? "compareWaveTerrainSearch"
                : contact_query      ? "compareTerrainSearch"
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

    GridResult repairProbe(Fixture &fixture, const TerrainView terrain, const std::array<float, 2u> point) {
        return contactProbe(
            fixture, terrain, {}, 0.0f, {}, 10'000'000, 0.0f, point, {}, {}, false, false, false, false, false, true);
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
        const TerrainView with_boundary{boundary, 1u, 1u, 6.0f, 7.0f};
        const TerrainView without_boundary{solid, 1u, 1u, 6.0f, 7.0f};
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

        void discover(rhi::ICommandEncoder &encoder, const bool first_destination_a = false) {
            PPR_TEST_ASSERT(not m_storage.discover(encoder, *m_hot, *m_cold, m_profile, first_destination_a, m_roots));
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

        const auto validation_setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
        const bool validation = not validation_setting or *validation_setting == "1";
        fixture.initialize(false, 32u, 32u, validation);

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
                const u32 first_generation = harness.m_storage.constants().m_generation;
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, true, harness.m_roots));
                poison(encoder, *buffers.m_payload[0u], 0x13579bdfu);

                particle.m_position_x = 3.5f;
                particle.m_position_y = 3.5f;
                harness.rows({&particle, 1u});
                harness.discover(encoder);
                harness.probe(encoder, witnesses, true, first_generation);
                previous_source = harness.m_results;
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, true, harness.m_roots));
                harness.probe(encoder, witnesses, true, harness.m_storage.constants().m_generation);
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
        static_assert(sizeof(GpuParticle) == 96u);
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
                const auto actual = std::bit_cast<std::array<u32, 24u>>(rows[row]);
                const auto admitted = std::bit_cast<std::array<u32, 24u>>(initial[row]);
                for (const float field: std::bit_cast<std::array<float, 24u>>(rows[row])) {
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
                _.log(
                    std::format("[dense1024] submit failed tick={} epoch={} error={}", tick, epoch, submitted.value()));
            }

            PPR_TEST_ASSERT(not submitted);
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            const auto complete = fixture.m_solver.pollTick();
            const auto failure = fixture.m_solver.failure();
            // 0 is publishable; a non-publishable (recoverable) verdict is reported
            // as 1 so the diagnostic never claims a rejection was a completed tick.
            const int verdict_code = not complete ? complete.error().value() : (*complete ? 0 : 1);
            std::println(
                status, "{},{},{},{},{},{},{},{}", tick, epoch, verdict_code, failure.m_flags, failure.m_node,
                failure.m_field, failure.m_particle, failure.m_body);
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
        _.log(
            std::format(
                "[dense1024] completed_ticks={} particles={} validation={}", ticks, particle_count, validation));
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
        PPR_TEST_ASSERT(created.m_query_count == 172u and created.m_completed_samples == 0u);
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

    // ------------------------------------------------------------------
    // matched sand tick cost diagnostics
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (sand_density_and_terrain_contact_instrumented_gpu_cost_diagnostic) {
        const char *const setting = std::getenv("PPR_PBMPM_COST_DIAGNOSTIC");
        const std::string_view enabled = setting != nullptr ? setting : "0";
        PPR_TEST_ASSERT(enabled == "0" or enabled == "1");
        if (enabled != "1") {
            _.log(std::format("SAND-GPU-COST skipped: set PPR_PBMPM_COST_DIAGNOSTIC=1"));
            return;
        }

        const char *const mode = std::getenv("PPR_PBMPM_COST_INSTRUMENTED");
        const std::string_view instrumentation = mode != nullptr ? mode : "1";
        PPR_TEST_ASSERT(instrumentation == "0" or instrumentation == "1");
        const bool instrumented = instrumentation == "1";
        const auto &clock = ITimerClock::steady();
        const ITimerClock *const observation_clock = instrumented ? &clock : nullptr;
        const u32 query_count = instrumented ? kDiagnosticQueryCount : 0u;
        const std::string_view sample_metric = instrumented ? "gpu_ms" : "completed_tick_wall_ms";
        const std::string_view summary_metric = instrumented ? "gpu" : "completed_tick_wall";
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, 128u, 128u, false, observation_clock, 0.0f);
        constexpr u32 particle_count = 4096u;
        const u32 warm_frames = instrumented ? 5u : 20u;
        const u32 sample_frames = instrumented ? 15u : 30u;
        static_assert(sizeof(GpuParticle) == 96u);
        const double frequency = instrumented
                                     ? static_cast<double>(fixture.m_rhi->getDevice().getInfo().timestampFrequency)
                                     : 0.0;
        PPR_TEST_ASSERT(not instrumented or (std::isfinite(frequency) and frequency > 0.0));
        for (const bool terrain_scene: {false, true}) {
            if (terrain_scene) {
                GpuParameters profile = fixture.m_solver.profile();
                profile.m_grid_width = 8192u;
                profile.m_grid_height = 32u;
                profile.m_particle_count = 0u;
                profile.m_shape_count = 0u;
                profile.m_gravity_strength = 9.81f / (32.0f / 3.0f);
                PPR_TEST_ASSERT(not fixture.m_solver.shutdown());
                PPR_TEST_ASSERT(not fixture.m_solver.initialize(
                    fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
                    profile, observation_clock));
            }

            const std::string_view workload = terrain_scene ? "terrain_strip" : "compressed_patch";
            const auto profile = fixture.m_solver.profile();
            PPR_TEST_ASSERT(profile.m_delta_time == 1.0f / 480.0f and profile.m_use_grid_volume_for_liquid == 1u);
            Array<GpuParticle> initial;
            initial.reserve(particle_count);
            for (u32 row = 0u; row < particle_count; ++row) {
                auto particle = restParticle();
                particle.m_position_x = terrain_scene ? 16.5f + float(row) : 32.0f + float(row % 64u) * 0.8f;
                particle.m_position_y = terrain_scene ? 11.5f : 32.0f + float(row / 64u) * 0.8f;
                particle.m_displacement_x = terrain_scene ? 0.01f : 0.0f;
                particle.m_material = static_cast<float>(ESourceMaterial::sand);
                particle.m_volume = 1.0f;
                particle.m_mass = 2.0f;
                particle.m_log_jp = 0.0f;
                initial.push_back(particle);
            }

            const auto masks = terrain_scene ? rectangleMasks(8192u, 16u, 8u) : std::vector<u32>{};
            const TerrainView terrain = terrain_scene ? TerrainView{masks, 8192u, 16u, 3.0f, 3.0f} : TerrainView{};
            Array<double> samples(sample_frames, 0.0);
            u64 previous_signal = 0u, previous_gpu = 0u, expected_bytes = 0u, expected_dispatches = 0u;
            _.log(
                std::format(
                    "SAND-GPU-COST-PROFILE workload={} rows={} grid={}x{} dt_substep_s={} source_substeps=8 source_iterations=2 volume_channel={} gravity_strength={} terrain={} source_cell_m=0.3333333333333333 reference_thickness_m=1 cold_volume=1 cold_mass=2 logJp=0 validation=false warm={} samples={} instrumented={} internal_timestamp_queries={} timestamp_payload_bytes={} ordinary_status_bytes=24 frozen_replacement_outside_timed_tick=true particle_snapshots_in_sample_loop=false candidate_shared_delta_model_bytes=512",
                    workload, initial.size(), profile.m_grid_width, profile.m_grid_height, profile.m_delta_time,
                    profile.m_use_grid_volume_for_liquid, profile.m_gravity_strength, terrain_scene, warm_frames,
                    sample_frames, instrumented, query_count, query_count * sizeof(u64)));
            for (u32 frame = 0u; frame < warm_frames + sample_frames; ++frame) {
                // Every timed batch starts with identical owned rows and terrain.
                // Replacement allocation/upload completion precedes either timing boundary.
                PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                    initial, {}, fixture.m_solver.accounting().m_epoch, terrain));
                fixture.m_solver.publishReplacement();
                const auto before = fixture.m_solver.accounting();
                const u64 epoch = before.m_epoch;
                const auto previous = fixture.m_solver.inspectTimingForTest();
                PPR_TEST_ASSERT(previous.m_query_count == query_count and not previous.m_error);
                if (not instrumented)
                    PPR_TEST_ASSERT(previous.m_completed_samples == 0u);

                // The no-query wall interval includes encoding/submission, the
                // actual completion wait, compact status collection and publication.
                // It excludes all replacement, profile and measurement bookkeeping.
                const auto wall_begin = instrumented ? TimePoint{} : clock.now();
                PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 8u, 2u));
                if (instrumented)
                    PPR_TEST_ASSERT(
                        fixture.m_solver.inspectTimingForTest().m_completed_samples == previous.m_completed_samples);

                PPR_TEST_ASSERT(not fixture.m_solver.drain());
                const auto ready = fixture.m_solver.pollTick();
                PPR_TEST_ASSERT(ready and *ready);
                fixture.m_solver.publishTick();
                const auto wall_end = instrumented ? TimePoint{} : clock.now();

                const auto timing = fixture.m_solver.inspectTimingForTest();
                PPR_TEST_ASSERT(not timing.m_error and timing.m_query_count == query_count);
                if (instrumented) {
                    PPR_TEST_ASSERT(timing.m_completed_samples == previous.m_completed_samples + 1u);
                    PPR_TEST_ASSERT(timing.m_epoch == epoch and timing.m_signal > previous_signal);
                    PPR_TEST_ASSERT(timing.m_first_gpu_timestamp >= previous_gpu);
                    PPR_TEST_ASSERT(timing.m_last_gpu_timestamp > timing.m_first_gpu_timestamp);
                    previous_signal = timing.m_signal;
                    previous_gpu = timing.m_last_gpu_timestamp;
                } else {
                    PPR_TEST_ASSERT(
                        timing.m_completed_samples == 0u and timing.m_epoch == 0u and timing.m_signal == 0u);
                    PPR_TEST_ASSERT(timing.m_first_gpu_timestamp == 0u and timing.m_last_gpu_timestamp == 0u);
                }

                const double elapsed_ms = instrumented
                                              ? double(timing.m_last_gpu_timestamp - timing.m_first_gpu_timestamp) *
                                                    1000.0 /
                                                    frequency
                                              : time::seconds(wall_end - wall_begin) * 1000.0;
                PPR_TEST_ASSERT(std::isfinite(elapsed_ms) and elapsed_ms > 0.0);

                const auto after = fixture.m_solver.accounting();
                PPR_TEST_ASSERT(after.m_epoch == epoch + 1u and after.m_particle_count == particle_count);
                PPR_TEST_ASSERT(after.m_substeps == 8u and after.m_last_readback_bytes == 6u * sizeof(u32));
                const u64 dispatches = after.m_dispatches - before.m_dispatches;
                if (frame < warm_frames)
                    continue;

                const u32 sample = frame - warm_frames;
                if (sample == 0u) {
                    expected_bytes = after.m_device_bytes;
                    expected_dispatches = dispatches;
                }

                PPR_TEST_ASSERT(after.m_device_bytes == expected_bytes and dispatches == expected_dispatches);
                samples[sample] = elapsed_ms;
                _.log(
                    std::format(
                        "SAND-GPU-COST-SAMPLE workload={} sample={} {}={} epoch={} diagnostic_signal={} device_bytes={} readback_capacity_bytes={} dispatches={} timestamp_first={} timestamp_last={} timestamp_frequency={}",
                        workload, sample, sample_metric, elapsed_ms, epoch, timing.m_signal, after.m_device_bytes,
                        after.m_readback_capacity_bytes, dispatches, timing.m_first_gpu_timestamp,
                        timing.m_last_gpu_timestamp, frequency));
            }

            std::ranges::sort(samples);
            const double median = sample_frames % 2u != 0u
                                      ? samples[sample_frames / 2u]
                                      : 0.5 * (samples[sample_frames / 2u - 1u] + samples[sample_frames / 2u]);
            _.log(
                std::format(
                    "SAND-GPU-COST workload={} rows={} warm={} samples={} {}_min_ms={} {}_median_ms={} {}_max_ms={} device_bytes={} dispatches={} instrumented={} validation=false",
                    workload, particle_count, warm_frames, sample_frames, summary_metric, samples.front(),
                    summary_metric, median, summary_metric, samples.back(), expected_bytes, expected_dispatches,
                    instrumented));
        }
    };

    // ------------------------------------------------------------------
    // iteration-count operating-point cost (opt-in, not a fidelity gate)
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (iteration_count_gpu_cost_diagnostic) {
        // Opt-in measurement, not a correctness gate: the repeated-use gate must
        // stay free of measurement work, and an absent switch is reported as an
        // explicit skip so a reader never mistakes it for evidence.
        const char *const setting = std::getenv("PPR_PBMPM_COST_DIAGNOSTIC");
        const std::string_view enabled = setting != nullptr ? setting : "0";
        PPR_TEST_ASSERT(enabled == "0" or enabled == "1");
        if (enabled != "1") {
            _.log(std::format("ITERATION-GPU-COST skipped: set PPR_PBMPM_COST_DIAGNOSTIC=1"));
            return;
        }

        // Same switch pair the sand cost diagnostic uses: instrumented runs also
        // report the internal timestamp span, uninstrumented runs measure only the
        // completed-tick wall interval without a query pool.
        const char *const mode = std::getenv("PPR_PBMPM_COST_INSTRUMENTED");
        const std::string_view instrumentation = mode != nullptr ? mode : "1";
        PPR_TEST_ASSERT(instrumentation == "0" or instrumentation == "1");
        const bool instrumented = instrumentation == "1";

        // The platform's injected steady clock: the same clock the fixture hands to
        // the solver. No sleep and no direct wall-clock read enters the measurement.
        const auto &clock = ITimerClock::steady();
        const u32 query_count = instrumented ? kDiagnosticQueryCount : 0u;

        constexpr u32 kGridEdge = 128u;
        constexpr u32 kRows = 4096u;
        constexpr u32 kSubsteps = kGameSubsteps;
        constexpr u32 kWarmTicks = 5u;
        constexpr u32 kSampleTicks = 30u;
        constexpr std::array<u32, 3u> kCounts{2u, 5u, 10u};
        static_assert(sizeof(GpuParticle) == 96u);

        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        // Validation layers are off for cost measurement, exactly as in the sand
        // cost diagnostic; they change the arithmetic by orders of magnitude. The
        // grid-volume channel stays at the production default (on) so the scene
        // measures the shipping arithmetic including the fourth scattered field.
        fixture.initialize(true, kGridEdge, kGridEdge, false, instrumented ? &clock : nullptr, 2.5f);
        const GpuParameters profile = fixture.m_solver.profile();
        const double frequency = instrumented
                                     ? static_cast<double>(fixture.m_rhi->getDevice().getInfo().timestampFrequency)
                                     : 0.0;
        PPR_TEST_ASSERT(not instrumented or (std::isfinite(frequency) and frequency > 0.0));

        // One fixed scene, reused unchanged for every K: 4096 rows on a 64x64 patch
        // of 0.8-cell spacing (all four source materials by row index, unit
        // mass/volume, logJp 0 so sand starts inside its stable branch), a 128x128
        // solver grid, a solid terrain floor over y=1..20 and one predicted box
        // boundary swept 0.25 cells across the eight substeps. Rows and terrain are
        // re-prepared outside every timed interval, so each measured tick starts
        // from identical owned state and the three K runs measure the same work.
        Array<GpuParticle> initial;
        initial.reserve(kRows);
        for (u32 row = 0u; row < kRows; ++row) {
            auto particle = restParticle();
            particle.m_position_x = 32.5f + static_cast<float>(row % 64u) * 0.8f;
            particle.m_position_y = 32.5f + static_cast<float>(row / 64u) * 0.8f;
            particle.m_material = static_cast<float>(row % 4u);
            particle.m_volume = 1.0f;
            particle.m_mass = 1.0f;
            particle.m_log_jp = 0.0f;
            initial.push_back(particle);
        }

        const auto masks = rectangleMasks(kGridEdge, kGridEdge, 20u);
        const TerrainView terrain{masks, kGridEdge, kGridEdge, 0.0f, 0.0f};

        std::array<GpuBodyBoundary, kSubsteps> boundaries{};
        for (u32 substep = 0u; substep < kSubsteps; ++substep) {
            auto &boundary = boundaries[substep];
            boundary.m_shape.m_shape = EStaticShape::box;
            boundary.m_shape.m_position_x = 64.0f;
            boundary.m_shape.m_position_y = 24.0f + 0.25f * static_cast<float>(substep);
            boundary.m_shape.m_half_size_x = 8.0f;
            boundary.m_shape.m_half_size_y = 1.0f;
            boundary.m_com_x = boundary.m_shape.m_position_x;
            boundary.m_com_y = boundary.m_shape.m_position_y;
            boundary.m_velocity_y = 0.25f / profile.m_delta_time;
        }

        // Zero brush: the final stage then takes its arithmetic bypass, which is the
        // production default for a tick without a pointer force.
        const GpuRadialForce force{};

        const auto median_of = [](Array<double> &values) {
            std::ranges::sort(values);
            const std::size_t middle = values.size() / 2u;
            return values.size() % 2u != 0u ? values[middle] : 0.5 * (values[middle - 1u] + values[middle]);
        };

        struct CostSample {
            double m_wall_median_ms{}, m_wall_max_ms{}, m_gpu_median_ms{}, m_gpu_max_ms{};
            u64 m_dispatches_per_tick{}, m_row_copy_bytes_per_tick{}, m_status_bytes_per_tick{};
            u64 m_device_bytes{}, m_full_snapshot_bytes{};
        };
        std::array<CostSample, kCounts.size()> samples{};
        _.log(
            std::format(
                "ITERATION-GPU-COST-PROFILE rows={} materials=row%4 grid={}x{} dt_substep_s={} "
                "gravity_strength={} volume_channel={} terrain_rows=1..20 boundary_boxes=1 substeps_per_tick={} "
                "cold_volume=1 cold_mass=1 logJp=0 validation=false warm={} samples={} instrumented={} "
                "internal_timestamp_queries={} timestamp_payload_bytes={} brush=disabled",
                kRows, kGridEdge, kGridEdge, profile.m_delta_time, profile.m_gravity_strength,
                profile.m_use_grid_volume_for_liquid, kSubsteps, kWarmTicks, kSampleTicks, instrumented, query_count,
                static_cast<u64>(query_count) * sizeof(u64)));

        // The runner surfaces _.log only for failing tests, so the same record also
        // goes to stdout for a passing run. Formatting follows the existing device
        // diagnostics in this file (`[matter-peak]` and `[isolated-liquid-pressure]`
        // use plain std::fprintf with explicit casts and a flushed line); only the
        // stream differs, per this measurement's evidence requirement.
        std::fprintf(
            stdout,
            "ITERATION-GPU-COST-PROFILE rows=%u materials=row%%4 grid=%ux%u dt_substep_s=%.9g gravity_strength=%.9g "
            "volume_channel=%u terrain_rows=1..20 boundary_boxes=1 substeps_per_tick=%u cold_volume=1 cold_mass=1 "
            "logJp=0 validation=false warm=%u samples=%u instrumented=%u internal_timestamp_queries=%u "
            "timestamp_payload_bytes=%llu brush=disabled\n",
            static_cast<unsigned>(kRows), static_cast<unsigned>(kGridEdge), static_cast<unsigned>(kGridEdge),
            static_cast<double>(profile.m_delta_time), static_cast<double>(profile.m_gravity_strength),
            static_cast<unsigned>(profile.m_use_grid_volume_for_liquid), static_cast<unsigned>(kSubsteps),
            static_cast<unsigned>(kWarmTicks), static_cast<unsigned>(kSampleTicks), static_cast<unsigned>(instrumented),
            static_cast<unsigned>(query_count), static_cast<unsigned long long>(query_count) * sizeof(u64));
        std::fflush(stdout);

        for (std::size_t index = 0u; index < kCounts.size(); ++index) {
            const u32 iterations = kCounts[index];
            const u64 expected_dispatches = static_cast<u64>(kSubsteps) * (9u + 2u * iterations);
            Array<double> wall_ms(kSampleTicks, 0.0);
            Array<double> gpu_ms(kSampleTicks, 0.0);
            u64 previous_signal = 0u, previous_gpu = 0u;

            for (u32 tick = 0u; tick < kWarmTicks + kSampleTicks; ++tick) {
                // Replacement allocation/upload completion precedes the timing
                // boundary, so only the resident batch is measured.
                PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                    initial, {}, fixture.m_solver.accounting().m_epoch, terrain));
                fixture.m_solver.publishReplacement();

                const auto before = fixture.m_solver.accounting();
                const u64 epoch = before.m_epoch;
                const auto previous = fixture.m_solver.inspectTimingForTest();
                PPR_TEST_ASSERT(previous.m_query_count == query_count and not previous.m_error);

                // The interval covers encoding, submission, the completion wait,
                // compact status collection and publication of the whole batch.
                const auto wall_begin = clock.now();
                PPR_TEST_ASSERT(
                    not fixture.m_solver.submitResidentTick(epoch, kSubsteps, iterations, boundaries, 1u, force));

                PPR_TEST_ASSERT(not fixture.m_solver.drain());

                const auto ready = fixture.m_solver.pollTick();
                PPR_TEST_ASSERT(ready and *ready);
                fixture.m_solver.publishTick();
                const auto wall_end = clock.now();

                const auto timing = fixture.m_solver.inspectTimingForTest();
                PPR_TEST_ASSERT(not timing.m_error and timing.m_query_count == query_count);
                if (instrumented) {
                    PPR_TEST_ASSERT(timing.m_completed_samples == previous.m_completed_samples + 1u);
                    PPR_TEST_ASSERT(timing.m_epoch == epoch and timing.m_signal > previous_signal);
                    PPR_TEST_ASSERT(
                        timing.m_last_gpu_timestamp > timing.m_first_gpu_timestamp and
                        timing.m_first_gpu_timestamp >= previous_gpu);
                    previous_signal = timing.m_signal;
                    previous_gpu = timing.m_last_gpu_timestamp;
                }

                const auto after = fixture.m_solver.accounting();
                PPR_TEST_ASSERT(after.m_epoch == epoch + 1u and after.m_particle_count == kRows);
                PPR_TEST_ASSERT(after.m_substeps == kSubsteps and after.m_last_readback_bytes == 6u * sizeof(u32));

                // Attribution: nine maintenance dispatches around discovery plus one
                // destination initialization and one fused dispatch per stage, in
                // each of the eight substeps. The delta therefore scales with K.
                const u64 dispatches = after.m_dispatches - before.m_dispatches;
                PPR_TEST_ASSERT(dispatches == expected_dispatches);

                if (tick < kWarmTicks)
                    continue;

                const u32 sample = tick - kWarmTicks;
                wall_ms[sample] = time::seconds(wall_end - wall_begin) * 1000.0;
                PPR_TEST_ASSERT(std::isfinite(wall_ms[sample]) and wall_ms[sample] > 0.0);
                if (instrumented) {
                    gpu_ms[sample] = static_cast<double>(timing.m_last_gpu_timestamp - timing.m_first_gpu_timestamp) *
                                     1000.0 /
                                     frequency;

                    PPR_TEST_ASSERT(std::isfinite(gpu_ms[sample]) and gpu_ms[sample] > 0.0);
                }

                // Copies per tick: the committed-to-working row copy the batch
                // encodes, then the six-word status readback. Publication itself
                // swaps ownership and copies nothing; a full frozen snapshot would
                // additionally copy m_full_snapshot_bytes.
                samples[index].m_dispatches_per_tick = dispatches;
                samples[index].m_row_copy_bytes_per_tick = after.m_last_particle_copy_bytes;
                samples[index].m_status_bytes_per_tick = after.m_last_readback_bytes;
                samples[index].m_device_bytes = after.m_device_bytes;
                samples[index].m_full_snapshot_bytes = after.m_readback_capacity_bytes;
            }

            samples[index].m_wall_median_ms = median_of(wall_ms);
            samples[index].m_wall_max_ms = std::ranges::max(wall_ms);
            if (instrumented) {
                samples[index].m_gpu_median_ms = median_of(gpu_ms);
                samples[index].m_gpu_max_ms = std::ranges::max(gpu_ms);
            }

            // Ratios are relative to the K = 2 reference, which is index 0 and is
            // already complete when this line runs.
            const CostSample &reference = samples.front();
            PPR_TEST_ASSERT(reference.m_wall_median_ms > 0.0);
            const double wall_ratio = samples[index].m_wall_median_ms / reference.m_wall_median_ms;
            const double gpu_ratio = instrumented ? samples[index].m_gpu_median_ms / reference.m_gpu_median_ms : 0.0;
            _.log(
                std::format(
                    "ITERATION-GPU-COST K={} rows={} grid={}x{} substeps={} warm={} samples={} instrumented={} "
                    "wall_median_ms={} wall_max_ms={} wall_ratio_k2={} gpu_median_ms={} gpu_max_ms={} gpu_ratio_k2={} "
                    "dispatches_per_tick={} expected_dispatches_per_tick={} row_copy_bytes_per_tick={} "
                    "status_readback_bytes_per_tick={} device_bytes={} full_snapshot_bytes={}",
                    iterations, kRows, kGridEdge, kGridEdge, kSubsteps, kWarmTicks, kSampleTicks, instrumented,
                    samples[index].m_wall_median_ms, samples[index].m_wall_max_ms, wall_ratio,
                    samples[index].m_gpu_median_ms, samples[index].m_gpu_max_ms, gpu_ratio,
                    samples[index].m_dispatches_per_tick, expected_dispatches, samples[index].m_row_copy_bytes_per_tick,
                    samples[index].m_status_bytes_per_tick, samples[index].m_device_bytes,
                    samples[index].m_full_snapshot_bytes));

            // Same evidence on stdout, in the compact machine-readable field set the
            // operating-point decision consumes.
            std::fprintf(
                stdout,
                "ITERATION-GPU-COST k=%u wall_median_ms=%.9g wall_max_ms=%.9g wall_ratio_k2=%.9g gpu_median_ms=%.9g "
                "gpu_max_ms=%.9g dispatches_per_tick=%llu row_copy_bytes_per_tick=%llu\n",
                static_cast<unsigned>(iterations), samples[index].m_wall_median_ms, samples[index].m_wall_max_ms,
                wall_ratio, samples[index].m_gpu_median_ms, samples[index].m_gpu_max_ms,
                static_cast<unsigned long long>(samples[index].m_dispatches_per_tick),
                static_cast<unsigned long long>(samples[index].m_row_copy_bytes_per_tick));
            std::fflush(stdout);
        }

        // Acceptance: every admitted count completed and published above, each
        // tick's dispatch delta is the K-scaled count exactly, and the heaviest
        // schedule is measurably slower than the lightest on this host. No absolute
        // millisecond value is asserted: these are operating-point observations.
        const u64 expected_row_copy = static_cast<u64>(kRows) * (sizeof(ParticleWrite) + sizeof(ParticleReadonly));
        for (std::size_t index = 0u; index < kCounts.size(); ++index) {
            PPR_TEST_ASSERT(
                samples[index].m_dispatches_per_tick == static_cast<u64>(kSubsteps) * (9u + 2u * kCounts[index]));
            PPR_TEST_ASSERT(samples[index].m_row_copy_bytes_per_tick == expected_row_copy);
        }

        PPR_TEST_ASSERT(
            samples[0u].m_dispatches_per_tick < samples[1u].m_dispatches_per_tick and
            samples[1u].m_dispatches_per_tick < samples[2u].m_dispatches_per_tick);
        PPR_TEST_ASSERT(samples[2u].m_wall_median_ms > samples[0u].m_wall_median_ms);
    };

    // ------------------------------------------------------------------
    // iteration-count correctness evidence (opt-in, not a contract yet)
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (iteration_count_correctness_diagnostic) {
        // Opt-in evidence, not a gate: the repeated-use gate carries no measurement
        // work, and an absent switch reports an explicit skip so nobody mistakes it
        // for evidence.
        const char *const setting = std::getenv("PPR_PBMPM_COST_DIAGNOSTIC");
        const std::string_view enabled = setting != nullptr ? setting : "0";
        PPR_TEST_ASSERT(enabled == "0" or enabled == "1");
        const char *const base_setting = std::getenv("PPR_PBMPM_CORRECTNESS_K2_ONLY");
        const std::string_view base_enabled = base_setting != nullptr ? base_setting : "0";
        PPR_TEST_ASSERT(base_enabled == "0" or base_enabled == "1");
        if (enabled != "1" and base_enabled != "1") {
            _.log(
                std::format(
                    "ITERATION-CORRECTNESS skipped: set PPR_PBMPM_COST_DIAGNOSTIC=1 or PPR_PBMPM_CORRECTNESS_K2_ONLY=1"));
            return;
        }

        constexpr u32 kGridEdge = 64u;
        constexpr u32 kHorizon = 240u;
        constexpr std::array<u32, 3u> all_counts{2u, 5u, 10u};
        const std::span<const u32> kCounts{all_counts.data(), base_enabled == "1" ? 1u : all_counts.size()};
        // K2_ONLY alone runs these identical scenes without enabling solver diagnostics.
        // Source at authoring: 078f48a24559521b43b6750b6fe856f7b7547824 + this leaf edit.
        // No numbers have yet been measured with these metrics. The measurement lane
        // must record the measured commit and separately capture the pre-schedule base.

        // The separate first-step relaxation fixture retains the historical shear
        // for branch separation only. It is not the load/release experiment.
        constexpr float kShear = 3.0f;

        // Shared residents: every scene is loaded whole, run for whole substeps and
        // read back through the real resident path. No scene depends on K.
        const auto load = [](Fixture &fixture, const std::span<const GpuParticle> rows) {
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(rows, {}, fixture.m_solver.accounting().m_epoch));
            fixture.m_solver.publishReplacement();
        };
        const auto run = [](Fixture &fixture, const u32 iterations, const u32 substeps,
                             const GpuRadialForce force = {}) {
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, substeps, iterations, {}, 0u, force));

            PPR_TEST_ASSERT(not fixture.m_solver.drain());

            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
            fixture.m_solver.publishTick();
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
            PPR_TEST_ASSERT(not fixture.m_solver.busy());

            return fixture.snapshot();
        };
        const auto inventory = [](const std::span<const GpuParticle> rows) {
            std::array<double, 2u> totals{};
            for (const auto &row: rows) {
                totals[0u] += static_cast<double>(row.m_mass);
                totals[1u] += static_cast<double>(row.m_volume);
            }

            return totals;
        };
        const auto assert_finite = [](const std::span<const GpuParticle> rows) {
            for (const auto &row: rows) {
                for (const float value: std::bit_cast<std::array<float, 24u>>(row)) {
                    PPR_TEST_ASSERT(std::isfinite(value));
                }
            }
        };
        const auto gradient_residual = [](const std::span<const GpuParticle> rows) {
            float value = 0.0f;
            for (const auto &row: rows) {
                const auto &gradient = row.m_deformation_gradient;
                value = std::max(
                    {value, std::abs(gradient.m_c0x - 1.0f), std::abs(gradient.m_c0y), std::abs(gradient.m_c1x),
                        std::abs(gradient.m_c1y - 1.0f)});
            }

            return value;
        };

        // Independent double-precision eigenvalues of C=F^T F, not forkSvd.
        // sigma_max=sqrt((C00+C11+hypot(C00-C11,2*C01))/2),
        // sigma_min=abs(det(F))/sigma_max (zero when sigma_max is zero).
        const auto singular_values = [](const double a, const double b, const double c, const double d) {
            const double x = a * a + b * b, y = c * c + d * d, z = a * c + b * d;
            const double high = std::sqrt(std::max(0.0, (x + y + std::hypot(x - y, 2.0 * z)) * 0.5));
            return std::array<double, 2u>{high, high == 0.0 ? 0.0 : std::abs(a * d - b * c) / high};
        };
        const auto report = [&](Fixture &fixture, const char *const property, const u32 iterations,
                                const char *const horizon, const u32 substeps,
                                const std::span<const GpuParticle> initial, const std::span<const GpuParticle> before,
                                const std::span<const GpuParticle> state) {
            PPR_TEST_ASSERT(state.size() == initial.size() and before.size() == initial.size());
            assert_finite(before);
            assert_finite(state);
            PPR_TEST_ASSERT(inventory(before) == inventory(initial));
            PPR_TEST_ASSERT(inventory(state) == inventory(initial));

            // Positional deformation is independent of the retained elastic F.
            // Equal-row 2D Procrustes fit removes translation and proper rotation,
            // but not scale, shear, reflection or particle rearrangement.
            double initial_x = 0.0, initial_y = 0.0, current_x = 0.0, current_y = 0.0;
            for (std::size_t row = 0u; row < state.size(); ++row) {
                initial_x += static_cast<double>(initial[row].m_position_x);
                initial_y += static_cast<double>(initial[row].m_position_y);
                current_x += static_cast<double>(state[row].m_position_x);
                current_y += static_cast<double>(state[row].m_position_y);
            }

            const double count = static_cast<double>(state.size());
            initial_x /= count;
            initial_y /= count;
            current_x /= count;
            current_y /= count;

            double dot = 0.0, cross = 0.0, squared_norms = 0.0;
            for (std::size_t row = 0u; row < state.size(); ++row) {
                const double x = static_cast<double>(initial[row].m_position_x) - initial_x;
                const double y = static_cast<double>(initial[row].m_position_y) - initial_y;
                const double u = static_cast<double>(state[row].m_position_x) - current_x;
                const double v = static_cast<double>(state[row].m_position_y) - current_y;
                dot += x * u + y * v;
                cross += x * v - y * u;
                squared_norms += x * x + y * y + u * u + v * v;
            }

            const double aligned_position_rms = std::sqrt(
                std::max(0.0, squared_norms - 2.0 * std::hypot(dot, cross)) / count);
            const double dt = static_cast<double>(fixture.m_solver.profile().m_delta_time);
            const double yield = std::exp(1.0 - static_cast<double>(fixture.m_solver.profile().m_plasticity));
            double energy = 0.0, total_displacement = 0.0, position_offset = 0.0, stretch_max = 0.0;
            u32 clamp_rows = 0u, yield_rows = 0u, sand_projection_rows = 0u;
            for (std::size_t row = 0u; row < state.size(); ++row) {
                const auto &p = state[row];
                const auto &f = p.m_deformation_gradient;
                const double a = static_cast<double>(f.m_c0x), b = static_cast<double>(f.m_c0y),
                             c = static_cast<double>(f.m_c1x), d = static_cast<double>(f.m_c1y);
                const auto sigma = singular_values(a, b, c, d);
                const double c00 = a * a + b * b - 1.0, c11 = c * c + d * d - 1.0;
                const double c01 = a * c + b * d;
                const double stretch = std::max({std::abs(c00), std::abs(c11), std::abs(c01)});
                // Recovery residual is ||F^T F-I||_F, not ||F-I|| or a positional
                // rigid-fit error. It ignores rotations; det separately exposes reflections.
                const double recovery = std::sqrt(c00 * c00 + c11 * c11 + 2.0 * c01 * c01);
                stretch_max = std::max(stretch_max, stretch);

                // D survives integration: reconstruct its trial (I+D)*F_previous
                // from adjacent snapshots. Counts refer only to this sampled substep,
                // not all clamp events during the intervening horizon. This independent
                // double-precision classification can differ from shader FP32 exactly
                // at a bound; it is not a GPU branch counter. Visco rescaling after
                // yield means retained singular values alone cannot identify clamping.
                const auto &g = before[row].m_deformation_gradient;
                const auto &delta = p.m_deformation_displacement;
                const double da = 1.0 + static_cast<double>(delta.m_c0x), db = static_cast<double>(delta.m_c0y);
                const double dc = static_cast<double>(delta.m_c1x), dd = 1.0 + static_cast<double>(delta.m_c1y);
                const double ta = da * static_cast<double>(g.m_c0x) + dc * static_cast<double>(g.m_c0y);
                const double tb = db * static_cast<double>(g.m_c0x) + dd * static_cast<double>(g.m_c0y);
                const double tc = da * static_cast<double>(g.m_c1x) + dc * static_cast<double>(g.m_c1y);
                const double td = db * static_cast<double>(g.m_c1x) + dd * static_cast<double>(g.m_c1y);
                const auto trial = singular_values(ta, tb, tc, td);
                // forkSvd's second value is signed for inverted trial gradients.
                const double signed_low = ta * td - tb * tc < 0.0 ? -trial[1u] : trial[1u];
                const bool clamped = trial[0u] >= 1e4 or signed_low <= 0.1;
                const double high = std::clamp(trial[0u], 0.1, 1e4);
                const double low = std::clamp(signed_low, 0.1, 1e4);
                const bool yielded = p.m_material == static_cast<float>(ESourceMaterial::visco) and
                                     (high >= yield or low <= 1.0 / yield);
                const double trace_e = std::log(high) + std::log(low) + static_cast<double>(before[row].m_log_jp);
                const double deviator = std::hypot(std::log(high) - trace_e * 0.5, std::log(low) - trace_e * 0.5);
                const double sin_phi = std::sin(
                    static_cast<double>(fixture.m_solver.profile().m_friction_angle) / 180.0 * 3.14159);
                const double alpha = std::sqrt(2.0 / 3.0) * 2.0 * sin_phi / (3.0 - sin_phi);
                const bool sand_projected =
                    p.m_material == static_cast<float>(ESourceMaterial::sand) and
                    (trace_e >= 0.0 or
                        deviator + (static_cast<double>(fixture.m_solver.profile().m_elasticity_ratio) + 1.0) *
                                       trace_e *
                                       alpha >
                            0.0);
                clamp_rows += clamped ? 1u : 0u;
                yield_rows += yielded ? 1u : 0u;
                sand_projection_rows += sand_projected ? 1u : 0u;

                const double displacement = std::hypot(
                    static_cast<double>(p.m_displacement_x), static_cast<double>(p.m_displacement_y));
                const double kinetic = 0.5 * static_cast<double>(p.m_mass) * displacement * displacement / (dt * dt);
                const double offset = std::hypot(
                    static_cast<double>(p.m_position_x) - static_cast<double>(initial[row].m_position_x),
                    static_cast<double>(p.m_position_y) - static_cast<double>(initial[row].m_position_y));
                energy += kinetic;
                total_displacement += displacement;
                position_offset += offset;
                std::fprintf(
                    stdout,
                    "ITERATION-CORRECTNESS property=%s k=%u horizon=%s substeps=%u row=%zu "
                    "sigma_max=%.9g sigma_min=%.9g stretch_max=%.9g recovery_frobenius=%.9g det_f=%.9g "
                    "rotation_sensitive_f_minus_i=%.9g integration_clamp=%u visco_yield_clamp=%u "
                    "sand_projection=%u kinetic_energy=%.9g displacement_magnitude=%.9g position_offset=%.9g\n",
                    property, iterations, horizon, substeps, row, sigma[0u], sigma[1u], stretch, recovery,
                    a * d - b * c, std::max({std::abs(a - 1.0), std::abs(b), std::abs(c), std::abs(d - 1.0)}),
                    clamped ? 1u : 0u, yielded ? 1u : 0u, sand_projected ? 1u : 0u, kinetic, displacement, offset);
            }

            const auto totals = inventory(state);
            std::fprintf(
                stdout,
                "ITERATION-CORRECTNESS property=%s k=%u horizon=%s substeps=%u rows=%zu "
                "stretch_max=%.9g integration_clamp_rows=%u visco_yield_clamp_rows=%u sand_projection_rows=%u "
                "kinetic_energy=%.9g total_displacement_magnitude=%.9g total_position_offset=%.9g "
                "aligned_position_rms=%.9g mass=%.9g volume=%.9g\n",
                property, iterations, horizon, substeps, state.size(), stretch_max, clamp_rows, yield_rows,
                sand_projection_rows, energy, total_displacement, position_offset, aligned_position_rms, totals[0u],
                totals[1u]);
            std::fflush(stdout);
        };

        {
            // Zero-gravity fixture: rest, compression, elastic and visco scenes.
            Fixture fixture;
            PPR_DEFER {
                fixture.shutdown();
            };
            fixture.initialize(true, kGridEdge, kGridEdge, false, nullptr, 0.0f);
            PPR_TEST_ASSERT(fixture.m_solver.profile().m_gravity_strength == 0.0f);

            // ---- rest drift: 16x16 unit-spacing liquid pool at density 1 ----
            Array<GpuParticle> pool;
            pool.reserve(16u * 16u);
            for (u32 row = 0u; row < 16u * 16u; ++row) {
                auto particle = restParticle();
                particle.m_position_x = 24.5f + static_cast<float>(row % 16u);
                particle.m_position_y = 24.5f + static_cast<float>(row / 16u);
                particle.m_material = static_cast<float>(ESourceMaterial::liquid);
                particle.m_liquid_density = 1.0f;
                particle.m_log_jp = 0.0f;
                pool.push_back(particle);
            }

            for (const u32 iterations: kCounts) {
                load(fixture, pool);
                const auto state = run(fixture, iterations, kHorizon);
                PPR_TEST_ASSERT(state.size() == pool.size());
                assert_finite(state);
                PPR_TEST_ASSERT(inventory(state) == inventory(pool));

                float max_displacement = 0.0f, density_min = std::numeric_limits<float>::max(), density_max = 0.0f;
                u32 moved = 0u;
                for (std::size_t row = 0u; row < state.size(); ++row) {
                    max_displacement = std::max(
                        {max_displacement, std::abs(state[row].m_displacement_x),
                            std::abs(state[row].m_displacement_y)});
                    density_min = std::min(density_min, state[row].m_liquid_density);
                    density_max = std::max(density_max, state[row].m_liquid_density);
                    if (std::abs(state[row].m_position_x - pool[row].m_position_x) > 1.0f or
                        std::abs(state[row].m_position_y - pool[row].m_position_y) > 1.0f) {
                        ++moved;
                    }
                }

                const auto totals = inventory(state);
                std::fprintf(
                    stdout,
                    "ITERATION-CORRECTNESS property=rest_drift k=%u substeps=%u rows=%u max_displacement=%.9g "
                    "moved_rows=%u density_min=%.9g density_max=%.9g mass=%.9g volume=%.9g\n",
                    static_cast<unsigned>(iterations), static_cast<unsigned>(kHorizon),
                    static_cast<unsigned>(state.size()), static_cast<double>(max_displacement),
                    static_cast<unsigned>(moved), static_cast<double>(density_min), static_cast<double>(density_max),
                    totals[0u], totals[1u]);
                std::fflush(stdout);
            }

            // ---- compression decay: one liquid row at density 0.75 ----
            auto compressed = restParticle();
            compressed.m_position_x = 32.5f;
            compressed.m_position_y = 32.5f;
            compressed.m_material = static_cast<float>(ESourceMaterial::liquid);
            compressed.m_liquid_density = 0.75f;
            compressed.m_log_jp = 0.0f;
            const std::array<GpuParticle, 1u> compressed_scene{compressed};

            for (const u32 iterations: kCounts) {
                load(fixture, compressed_scene);
                const auto after_1 = run(fixture, iterations, 1u);
                const auto after_8 = run(fixture, iterations, 7u);
                const auto after_64 = run(fixture, iterations, 56u);
                PPR_TEST_ASSERT(after_1.size() == 1u and after_8.size() == 1u and after_64.size() == 1u);
                assert_finite(after_64);
                PPR_TEST_ASSERT(inventory(after_64) == inventory(compressed_scene));

                const float error_1 = std::abs(after_1[0u].m_liquid_density - 1.0f);
                const float error_8 = std::abs(after_8[0u].m_liquid_density - 1.0f);
                const float error_64 = std::abs(after_64[0u].m_liquid_density - 1.0f);
                const auto totals = inventory(after_64);
                std::fprintf(
                    stdout,
                    "ITERATION-CORRECTNESS property=compression_decay k=%u substeps=%u rows=1 density=%.9g "
                    "density_error_s1=%.9g density_error_s8=%.9g density_error_s64=%.9g density_s64=%.9g "
                    "mass=%.9g volume=%.9g\n",
                    static_cast<unsigned>(iterations), static_cast<unsigned>(kHorizon),
                    static_cast<double>(compressed.m_liquid_density), static_cast<double>(error_1),
                    static_cast<double>(error_8), static_cast<double>(error_64),
                    static_cast<double>(after_64[0u].m_liquid_density), totals[0u], totals[1u]);
                std::fflush(stdout);
            }

            // ---- first-step branch separation and solver-driven load/release ----
            const auto sheared_patch = [](const ESourceMaterial material, const float shear) {
                Array<GpuParticle> patch;
                patch.reserve(8u * 8u);
                for (u32 row = 0u; row < 8u * 8u; ++row) {
                    auto particle = restParticle();
                    particle.m_position_x = 24.5f + static_cast<float>(row % 8u);
                    particle.m_position_y = 24.5f + static_cast<float>(row / 8u);
                    particle.m_material = static_cast<float>(material);
                    particle.m_log_jp = 0.0f;
                    particle.m_deformation_gradient = {1.0f, 0.0f, shear, 1.0f};
                    patch.push_back(particle);
                }

                return patch;
            };
            const Array<GpuParticle> elastic_scene = sheared_patch(ESourceMaterial::elastic, kShear);
            const Array<GpuParticle> visco_scene = sheared_patch(ESourceMaterial::visco, kShear);

            for (const u32 iterations: kCounts) {
                float residual[2u]{};
                const std::array<const Array<GpuParticle> *, 2u> scenes{&elastic_scene, &visco_scene};
                const std::array<const char *, 2u> names{"elastic_relaxation", "visco_relaxation"};
                for (std::size_t scene = 0u; scene < scenes.size(); ++scene) {
                    load(fixture, *scenes[scene]);
                    const auto after_1 = run(fixture, iterations, 1u);
                    report(
                        fixture, names[scene], iterations, "early_separation", 1u, *scenes[scene], *scenes[scene],
                        after_1);
                    residual[scene] = gradient_residual(after_1);
                }

                // Structural visco-vs-elastic check. The visco yield clamp binds on the
                // first integration (3.303 > e = 2.718 on the major singular value)
                // while the elastic branch has no such projection, so the two retained
                // deformations must differ by more than 5 % of the larger one. This is
                // a branch-identity claim, not a recovery or fidelity tolerance.
                const float larger = std::max(residual[0u], residual[1u]);
                PPR_TEST_ASSERT(larger > 0.0f);
                PPR_TEST_ASSERT(std::abs(residual[0u] - residual[1u]) > 0.05f * larger);

                // Initially stress-free patch, loaded only through the resident
                // integration force. Off-centre inward brush gives nonuniform load.
                // 16 loaded substeps, then 224 force-free substeps; snapshots at
                // 16 (load end), 128 (release mid), 240 (release end). No state reset
                // on release. Acceleration is in cells/s^2; dt is the source dt.
                constexpr GpuRadialForce brush{24.0f, 28.0f, 12.0f, -2400.0f};
                const std::array<ESourceMaterial, 2u> materials{ESourceMaterial::elastic, ESourceMaterial::visco};
                const std::array<const char *, 2u> load_names{"elastic_load_release", "visco_load_release"};
                const std::array<const char *, 3u> horizons{"load_end", "release_mid", "release_end"};
                constexpr std::array<u32, 3u> ends{16u, 128u, 240u};
                for (std::size_t scene = 0u; scene < materials.size(); ++scene) {
                    const auto initial = sheared_patch(materials[scene], 0.0f);
                    load(fixture, initial);
                    u32 elapsed = 0u;
                    for (std::size_t horizon = 0u; horizon < ends.size(); ++horizon) {
                        const GpuRadialForce force = horizon == 0u ? brush : GpuRadialForce{};
                        const auto before = run(fixture, iterations, ends[horizon] - elapsed - 1u, force);
                        const auto state = run(fixture, iterations, 1u, force);
                        report(
                            fixture, load_names[scene], iterations, horizons[horizon], ends[horizon], initial, before,
                            state);
                        elapsed = ends[horizon];
                    }
                }
            }
        }

        {
            // Gravity fixture: the granular column settles against the guardian floor.
            Fixture fixture;
            PPR_DEFER {
                fixture.shutdown();
            };
            fixture.initialize(true, kGridEdge, kGridEdge, false, nullptr, 2.5f);
            PPR_TEST_ASSERT(fixture.m_solver.profile().m_gravity_strength > 0.0f);

            // ---- granular settling: 8 wide x 16 tall sand column ----
            Array<GpuParticle> column;
            column.reserve(8u * 16u);
            for (u32 row = 0u; row < 8u * 16u; ++row) {
                auto particle = restParticle();
                particle.m_position_x = 28.5f + static_cast<float>(row % 8u);
                particle.m_position_y = 20.5f + static_cast<float>(row / 8u);
                particle.m_material = static_cast<float>(ESourceMaterial::sand);
                particle.m_log_jp = 0.0f;
                column.push_back(particle);
            }

            for (const u32 iterations: kCounts) {
                load(fixture, column);
                const auto before = run(fixture, iterations, kHorizon - 1u);
                const auto state = run(fixture, iterations, 1u);
                report(fixture, "granular_elastic_state", iterations, "settle_end", kHorizon, column, before, state);
                PPR_TEST_ASSERT(state.size() == column.size());
                assert_finite(state);
                PPR_TEST_ASSERT(inventory(state) == inventory(column));

                float top = -std::numeric_limits<float>::max(), bottom = std::numeric_limits<float>::max();
                u32 moved = 0u;
                for (std::size_t row = 0u; row < state.size(); ++row) {
                    top = std::max(top, state[row].m_position_y);
                    bottom = std::min(bottom, state[row].m_position_y);
                    if (std::abs(state[row].m_position_x - column[row].m_position_x) > 1.0f or
                        std::abs(state[row].m_position_y - column[row].m_position_y) > 1.0f) {
                        ++moved;
                    }
                }

                const auto totals = inventory(state);
                std::fprintf(
                    stdout,
                    "ITERATION-CORRECTNESS property=granular_settling k=%u substeps=%u rows=%u top_height=%.9g "
                    "bottom_height=%.9g moved_rows=%u mass=%.9g volume=%.9g\n",
                    static_cast<unsigned>(iterations), static_cast<unsigned>(kHorizon),
                    static_cast<unsigned>(state.size()), static_cast<double>(top), static_cast<double>(bottom),
                    static_cast<unsigned>(moved), totals[0u], totals[1u]);
                std::fflush(stdout);
            }
        }
    };

    // ------------------------------------------------------------------
    // resident absolute-FP32 and fixed-point precision characterization
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (resident_precision_and_quantization_diagnostic) {
        // Opt-in evidence, not a gate: the repeated-use gate carries no measurement
        // work, and an absent switch reports an explicit skip so a reader never
        // mistakes it for evidence.
        const char *const setting = std::getenv("PPR_PBMPM_COST_DIAGNOSTIC");
        const std::string_view enabled = setting != nullptr ? setting : "0";
        PPR_TEST_ASSERT(enabled == "0" or enabled == "1");
        if (enabled != "1") {
            _.log(std::format("RESIDENT-PRECISION skipped: set PPR_PBMPM_COST_DIAGNOSTIC=1"));
            return;
        }

        // Case matrix, measured in full and therefore not reduced. Four absolute
        // coordinates cross three requested physical speeds, three normalized
        // masses and two subcell phases; each axis bounds one loss channel:
        //   coordinate - the absolute-FP32 position ULP at the row's magnitude;
        //   speed      - the per-substep translation against that ULP;
        //   mass       - the fixed-point momentum quantum 1 / multiplier relative
        //                to mass * displacement in the gather/scatter ratio;
        //   phase      - node centre (0) versus midway between node centres (0.5).
        // At initial scatter the smallest mass keeps every contributing node's
        // quantized mass above the 1e-5 support guard: the minimum 2D weight is
        // 1/64 at centre (0.001/64 = 1.5625e-5), or 1/16 halfway. Subsequent
        // motion changes the weights; these bounds describe the seeded phases.
        constexpr double kCellMetres = 1.0 / 3.0; // plan fixed cell size dx = 1/3 m
        constexpr float kInteriorY = 16.5f;
        constexpr std::array<float, 4u> kCoordinates{6.5f, 512.5f, 1024.5f, 4096.5f};
        constexpr std::array<double, 3u> kSpeeds{0.001, 0.01, 0.1};
        constexpr std::array<float, 3u> kMasses{1.0f, 0.01f, 0.001f};
        constexpr std::array<float, 2u> kPhases{0.0f, 0.5f};
        // One physical second at dt = 1/480 is 480 substeps, and the production
        // batch is kGameSubsteps = 8 substeps, so the long horizon is 60 whole
        // K=5 batches. The widest coordinate plus its phase and the longest
        // translation stay inside the guardian interval [3, width - 4].
        constexpr u32 kLongSubsteps = 480u;
        constexpr u32 kBatchSubsteps = kGameSubsteps;
        constexpr u32 kGridWidth = 4104u;
        constexpr u32 kGridHeight = 40u;
        static_assert(kLongSubsteps % kBatchSubsteps == 0u);
        static_assert(4096.5 + 0.5 + 0.3 < static_cast<double>(kGridWidth) - 4.0);

        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        // Zero gravity, no terrain, no shapes, no brush and no grid-volume
        // feedback: single rows characterize repeated quantized transport; the
        // shear patch also includes liquid constitutive and pressure relaxation.
        fixture.initialize(false, kGridWidth, kGridHeight, false, nullptr, 0.0f);

        const GpuParameters profile = fixture.m_solver.profile();
        PPR_TEST_ASSERT(profile.m_gravity_strength == 0.0f);
        PPR_TEST_ASSERT(profile.m_use_grid_volume_for_liquid == 0u);
        PPR_TEST_ASSERT(profile.m_shape_count == 0u);
        const double dt = static_cast<double>(profile.m_delta_time);
        const double multiplier = static_cast<double>(profile.m_fixed_point_multiplier);
        PPR_TEST_ASSERT(dt > 0.0 and multiplier >= 1.0);

        // One complete batch: submit, drain, classify and publish. The collected
        // status must be publishable and the failure record empty, so a rejected
        // or partial batch is never reported as a measurement.
        const auto submit_batch = [](Fixture &fixture, const u32 iterations, const u32 substeps) {
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, substeps, iterations));

            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
            PPR_TEST_ASSERT(fixture.m_solver.failure().m_flags == 0u);

            fixture.m_solver.publishTick();
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
        };
        const auto load = [](Fixture &fixture, const std::span<const GpuParticle> rows) {
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(rows, {}, fixture.m_solver.accounting().m_epoch));
            fixture.m_solver.publishReplacement();
        };
        // Whole complete batches only; the single readback happens after the last
        // batch, so the horizon stays the only snapshot in the measurement.
        const auto advance = [&](Fixture &fixture, const u32 iterations, const u32 substeps) {
            PPR_TEST_ASSERT(substeps != 0u);
            u32 remaining = substeps;
            while (remaining > kBatchSubsteps) {
                submit_batch(fixture, iterations, kBatchSubsteps);
                remaining -= kBatchSubsteps;
            }

            submit_batch(fixture, iterations, remaining);
            return fixture.snapshot();
        };
        const auto inventory = [](const std::span<const GpuParticle> rows) {
            std::array<double, 2u> totals{};
            for (const auto &row: rows) {
                totals[0u] += static_cast<double>(row.m_mass);
                totals[1u] += static_cast<double>(row.m_volume);
            }

            return totals;
        };
        const auto assert_finite = [](const std::span<const GpuParticle> rows) {
            for (const auto &row: rows) {
                for (const float value: std::bit_cast<std::array<float, 24u>>(row))
                    PPR_TEST_ASSERT(std::isfinite(value));
            }
        };

        // Independent double-precision reference, from the profile constants and
        // the plan cell size, not from the shader's expression. The solver stores
        // the candidate displacement in cells and integrates
        // `position.x += displacement.x` once per substep, so a requested physical
        // speed v predicts
        //
        //   displacement_per_substep_cells = v * dt / h
        //   expected_cells(N)              = N * v * dt / h
        //   expected_metres(N)             = N * v * dt
        //
        // with h = 1/3 m and dt read from the profile. This reference applies
        // neither the fixed-point encode (step 1 / multiplier cells) nor FP32
        // position addition, so both losses appear as measured - expected. The
        // production encode truncates toward zero (`int(value * multiplier)`), so
        // the residual carries a directed bias on top of a nearest-rounding half
        // quantum.
        const auto expected_cells = [dt](const double speed, const u32 substeps) {
            return static_cast<double>(substeps) * speed * dt / kCellMetres;
        };
        const auto position_ulp = [](const float coordinate) {
            return static_cast<double>(std::nextafter(coordinate, std::numeric_limits<float>::infinity())) -
                   static_cast<double>(coordinate);
        };

        // The seeded candidate displacement is the requested speed's per-substep
        // translation in cells. It is the only initial motion the row carries; the
        // solver re-derives it from the fixed-point grid every substep.
        const auto make_row = [&](const float x, const float y, const float mass, const double speed) {
            auto row = restParticle();
            row.m_position_x = x;
            row.m_position_y = y;
            row.m_material = static_cast<float>(ESourceMaterial::liquid);
            row.m_mass = mass;
            row.m_volume = 1.0f;
            row.m_liquid_density = 1.0f;
            row.m_log_jp = 0.0f;
            row.m_displacement_x = static_cast<float>(expected_cells(speed, 1u));
            row.m_displacement_y = 0.0f;
            return row;
        };

        struct HorizonMeasurement {
            double m_measured_cells{}, m_expected_cells{}, m_represented_cells{};
        };
        u32 reported_cases = 0u;
        const auto report_single = [&](const float coordinate, const float phase, const double speed, const float mass,
                                       const u32 iterations, const u32 substeps,
                                       const std::span<const GpuParticle> initial,
                                       const std::span<const GpuParticle> state, const double one_step_cells) {
            PPR_TEST_ASSERT(initial.size() == 1u and state.size() == 1u);
            assert_finite(state);
            PPR_TEST_ASSERT(inventory(state) == inventory(initial));

            const double expected = expected_cells(speed, substeps);
            PPR_TEST_ASSERT(expected > 0.0);
            const double initial_x = static_cast<double>(initial[0u].m_position_x);
            const double measured = static_cast<double>(state[0u].m_position_x) - initial_x;
            const double represented = std::hypot(
                static_cast<double>(state[0u].m_displacement_x), static_cast<double>(state[0u].m_displacement_y));
            const double ulp = position_ulp(initial[0u].m_position_x);
            const double mass_value = static_cast<double>(initial[0u].m_mass);
            // Retained candidate-displacement momentum m * d, the quantity the
            // fixed-point scatter accumulates per substep.
            const double momentum_initial = mass_value * static_cast<double>(initial[0u].m_displacement_x);
            const double momentum_final = mass_value * static_cast<double>(state[0u].m_displacement_x);
            const double drift = momentum_initial != 0.0
                                     ? std::abs(momentum_final - momentum_initial) / std::abs(momentum_initial)
                                     : 0.0;
            const auto totals = inventory(state);
            const bool frozen = measured == 0.0;

            // Deviation from ideal ballistic transport, not a unit conversion.
            // The effective cumulative loss compares mean transport per substep
            // with the independently restarted one-step measurement. It is not
            // an instantaneous velocity-decay estimate. Undefined baselines and
            // the one-step horizon report NaN rather than an invented zero loss.
            const double ballistic_fraction = measured / expected;
            const double effective_loss = substeps > 1u and one_step_cells > 0.0 and measured >= 0.0
                                              ? 1.0 - std::pow(
                                                          measured / (static_cast<double>(substeps) * one_step_cells),
                                                          1.0 / static_cast<double>(substeps - 1u))
                                              : std::numeric_limits<double>::quiet_NaN();

            std::fprintf(
                stdout,
                "PRECISION-QUANTIZATION scene=single coordinate_cells=%.9g phase=%.9g speed_m_s=%.9g mass=%.9g "
                "substeps=%u iterations=%u expected_cells=%.9g expected_m=%.9g measured_cells=%.9g "
                "measured_m=%.9g represented_cells=%.9g represented_m=%.9g position_ulp_cells=%.9g "
                "position_ulp_m=%.9g quantization_residual_quanta=%.9g momentum_initial=%.9g "
                "momentum_final=%.9g momentum_drift_relative=%.9g inventory_mass=%.9g inventory_volume=%.9g "
                "ballistic_transport_fraction=%.9g ballistic_deviation_relative=%.9g "
                "effective_mean_transport_loss_per_substep=%.9g frozen=%u\n",
                static_cast<double>(coordinate), static_cast<double>(phase), speed, static_cast<double>(mass),
                static_cast<unsigned>(substeps), static_cast<unsigned>(iterations), expected, expected * kCellMetres,
                measured, measured * kCellMetres, represented, represented * kCellMetres, ulp, ulp * kCellMetres,
                (measured - expected) * multiplier, momentum_initial, momentum_final, drift, totals[0u], totals[1u],
                ballistic_fraction, ballistic_fraction - 1.0, effective_loss, frozen ? 1u : 0u);
            std::fflush(stdout);

            ++reported_cases;
            return HorizonMeasurement{measured, expected, represented};
        };

        // Affine control geometry: a 2 x 1 cell patch centred on the widest
        // coordinate, carrying the symmetric shear field u(r) = L r with
        // L = [[0, s], [s, 0]] about the patch centre. Per-particle d is the field
        // at that particle and the retained affine term is D = L. The reported
        // shear-field moment is not total APIC angular momentum. The patch is
        // deliberately not square: cross(r, L r) =
        // s (x^2 - y^2), which has one sign over this patch because |x| = 1 against
        // |y| = 0.5, so a shear with zero vorticity still produces a nonzero signed
        // moment; a square patch would cancel it identically.
        constexpr float kAffineCentreX = 4096.5f;
        constexpr float kAffineCentreY = kInteriorY;
        constexpr float kAffineMass = 1.0f;
        constexpr double kShearRate = 6.25e-4;
        constexpr std::array<std::array<float, 2u>, 4u> kAffineOffsets{
            {{-1.0f, -0.5f}, {1.0f, -0.5f}, {-1.0f, 0.5f}, {1.0f, 0.5f}}};
        const auto patch_centre = [](const std::span<const GpuParticle> rows) {
            std::array<double, 3u> centre{}; // mass, then mass-weighted x and y
            for (const auto &row: rows) {
                const double mass = static_cast<double>(row.m_mass);
                centre[0u] += mass;
                centre[1u] += mass * static_cast<double>(row.m_position_x);
                centre[2u] += mass * static_cast<double>(row.m_position_y);
            }

            PPR_TEST_ASSERT(centre[0u] > 0.0);
            centre[1u] /= centre[0u];
            centre[2u] /= centre[0u];
            return centre;
        };
        // sum m * cross(r, M r) about the patch centre, signed and unsigned.
        // `retained` selects the row's own deformation displacement D; otherwise
        // the declared field L is the reference, evaluated on the same measured
        // geometry. Because the 2 x 1 patch gives the shear's cross term one sign,
        // both totals describe the field moment; the reference is the undepleted
        // declared field on measured geometry, not a transfer-retention prediction.
        const auto affine_moment = [&](const std::span<const GpuParticle> rows, const bool retained) {
            const auto centre = patch_centre(rows);
            std::array<double, 2u> moment{};
            for (const auto &row: rows) {
                const double x = static_cast<double>(row.m_position_x) - centre[1u];
                const double y = static_cast<double>(row.m_position_y) - centre[2u];
                const auto &field = row.m_deformation_displacement;
                const double c0x = retained ? static_cast<double>(field.m_c0x) : 0.0;
                const double c0y = retained ? static_cast<double>(field.m_c0y) : kShearRate;
                const double c1x = retained ? static_cast<double>(field.m_c1x) : kShearRate;
                const double c1y = retained ? static_cast<double>(field.m_c1y) : 0.0;
                const double cross = x * (c0y * x + c1y * y) - y * (c0x * x + c1x * y);
                const double mass = static_cast<double>(row.m_mass);
                moment[0u] += mass * cross;
                moment[1u] += mass * std::abs(cross);
            }

            return moment;
        };
        const auto report_affine = [&](const u32 iterations, const u32 substeps,
                                       const std::span<const GpuParticle> initial,
                                       const std::span<const GpuParticle> state) {
            PPR_TEST_ASSERT(state.size() == kAffineOffsets.size() and initial.size() == state.size());
            assert_finite(state);
            PPR_TEST_ASSERT(inventory(state) == inventory(initial));

            const auto measured = affine_moment(state, true);
            const auto reference = affine_moment(state, false);
            PPR_TEST_ASSERT(reference[1u] > 0.0);

            const auto initial_centre = patch_centre(initial);
            const auto state_centre = patch_centre(state);
            const double centre_drift = std::hypot(
                state_centre[1u] - initial_centre[1u], state_centre[2u] - initial_centre[2u]);
            const double retained_ratio = measured[1u] / reference[1u];
            // End-to-end liquid shear decay includes constitutive relaxation at
            // every stage, pressure relaxation, quantization and absolute-position
            // rounding. This ratio does not attribute loss to APIC transfer.
            const bool frozen = measured[1u] == 0.0;
            const auto totals = inventory(state);

            std::fprintf(
                stdout,
                "PRECISION-QUANTIZATION scene=affine_shear centre_cells=%.9g shear_rate_per_cell=%.9g "
                "substeps=%u iterations=%u absolute_shear_field_moment_cells2=%.9g signed_shear_field_moment_cells2=%.9g "
                "undepleted_shear_field_moment_cells2=%.9g absolute_shear_field_moment_m2_s=%.9g "
                "end_to_end_shear_moment_ratio=%.9g centre_drift_cells=%.9g "
                "inventory_mass=%.9g inventory_volume=%.9g frozen=%u\n",
                static_cast<double>(kAffineCentreX), kShearRate, static_cast<unsigned>(substeps),
                static_cast<unsigned>(iterations), measured[1u], measured[0u], reference[1u],
                measured[1u] * kCellMetres * kCellMetres / dt, retained_ratio, centre_drift, totals[0u], totals[1u],
                frozen ? 1u : 0u);
            std::fflush(stdout);

            ++reported_cases;
        };

        std::fprintf(
            stdout,
            "PRECISION-PROFILE grid=%ux%u cell_m=%.9g dt_s=%.9g fixed_point_multiplier=%.9g gravity_strength=0 "
            "volume_channel=0 terrain=none shapes=0 brush=disabled batch_substeps=%u long_substeps=%u "
            "schedule_k=%u control_k=%u\n",
            static_cast<unsigned>(kGridWidth), static_cast<unsigned>(kGridHeight), kCellMetres, dt, multiplier,
            static_cast<unsigned>(kBatchSubsteps), static_cast<unsigned>(kLongSubsteps),
            static_cast<unsigned>(kDefaultIterations), static_cast<unsigned>(kSourceIterations));

        // Initial scatter truncates weighted mass * displacement at 1/multiplier.
        // Thus the first nonzero momentum quantum needs v >= h/(dt*multiplier*w*m),
        // whose numerator is 1.6e-5 m/s for this profile. List each nonzero 2D
        // weight, since the first supported node and the last differ substantially.
        const double scatter_speed_numerator = kCellMetres / (dt * multiplier);
        std::fprintf(
            stdout, "PRECISION-PROFILE initial_scatter_speed_numerator_m_s=%.9g node_mass_cutoff=1e-5\n",
            scatter_speed_numerator);
        for (const float mass: kMasses) {
            for (const float phase: kPhases) {
                const std::array<double, 3u> weights = phase == 0.0f ? std::array{1.0 / 64.0, 3.0 / 32.0, 9.0 / 16.0}
                                                                     : std::array{1.0 / 16.0, 3.0 / 8.0, 0.0};
                for (const double weight: weights) {
                    if (weight == 0.0)
                        continue;

                    std::fprintf(
                        stdout,
                        "PRECISION-PROFILE mass=%.9g phase=%.9g weight=%.9g initial_scatter_velocity_floor_m_s=%.9g\n",
                        static_cast<double>(mass), static_cast<double>(phase), weight,
                        scatter_speed_numerator / (weight * static_cast<double>(mass)));
                }
            }
        }

        // Below half an absolute-position ULP, a positive per-step increment
        // rounds away; equality depends on tie-to-even. This is a local threshold,
        // not a prediction after repeated transfers have depleted displacement.
        for (const float coordinate: kCoordinates) {
            for (const float phase: kPhases) {
                std::fprintf(
                    stdout, "PRECISION-PROFILE coordinate_cells=%.9g phase=%.9g half_ulp_speed_m_s=%.9g\n",
                    static_cast<double>(coordinate), static_cast<double>(phase),
                    0.5 * position_ulp(coordinate + phase) * kCellMetres / dt);
            }
        }

        std::fflush(stdout);

        // Representative case for the schedule control: widest coordinate, middle
        // speed, unit mass, node-centre phase. It is also one cell of the matrix
        // above, so its K=5 long-horizon measurement is captured there.
        constexpr float kControlCoordinate = kCoordinates.back();
        constexpr float kControlPhase = kPhases.front();
        constexpr float kControlMass = kMasses.front();
        constexpr double kControlSpeed = kSpeeds[1u];
        HorizonMeasurement k5_control{};

        for (const float coordinate: kCoordinates) {
            for (const float phase: kPhases) {
                for (const double speed: kSpeeds) {
                    for (const float mass: kMasses) {
                        const std::array<GpuParticle, 1u> initial{
                            make_row(coordinate + phase, kInteriorY, mass, speed)};

                        // Both horizons restart from identical published state: the
                        // long horizon must be an integral number of whole batches.
                        load(fixture, initial);
                        const auto after_one = advance(fixture, kDefaultIterations, 1u);
                        const auto one = report_single(
                            coordinate, phase, speed, mass, kDefaultIterations, 1u, initial, after_one, 0.0);

                        load(fixture, initial);
                        const auto after_long = advance(fixture, kDefaultIterations, kLongSubsteps);
                        const auto measurement = report_single(
                            coordinate, phase, speed, mass, kDefaultIterations, kLongSubsteps, initial, after_long,
                            one.m_measured_cells);
                        const bool control_case = coordinate == kControlCoordinate and
                                                  phase == kControlPhase and
                                                  speed == kControlSpeed and
                                                  mass == kControlMass;
                        if (control_case)
                            k5_control = measurement;
                    }
                }
            }
        }

        {
            // K=2 control on the same scene. The schedule changes the number of
            // gather/scatter stages per substep, so an unchanged measurement here
            // shows the reported loss is a representation limit and not a K=5
            // artifact. Equality is not asserted; the difference is reported.
            const std::array<GpuParticle, 1u> initial{
                make_row(kControlCoordinate + kControlPhase, kInteriorY, kControlMass, kControlSpeed)};
            load(fixture, initial);
            const auto after_one = advance(fixture, kSourceIterations, 1u);
            const auto one = report_single(
                kControlCoordinate, kControlPhase, kControlSpeed, kControlMass, kSourceIterations, 1u, initial,
                after_one, 0.0);

            load(fixture, initial);
            const auto after_long = advance(fixture, kSourceIterations, kLongSubsteps);
            const auto k2_long = report_single(
                kControlCoordinate, kControlPhase, kControlSpeed, kControlMass, kSourceIterations, kLongSubsteps,
                initial, after_long, one.m_measured_cells);

            const double delta_cells = k5_control.m_measured_cells - k2_long.m_measured_cells;
            std::fprintf(
                stdout,
                "PRECISION-SCHEDULE-CONTROL coordinate_cells=%.9g phase=%.9g speed_m_s=%.9g mass=%.9g substeps=%u "
                "k5_measured_cells=%.9g k2_measured_cells=%.9g delta_cells=%.9g delta_quanta=%.9g "
                "k5_represented_cells=%.9g k2_represented_cells=%.9g\n",
                static_cast<double>(kControlCoordinate), static_cast<double>(kControlPhase), kControlSpeed,
                static_cast<double>(kControlMass), static_cast<unsigned>(kLongSubsteps), k5_control.m_measured_cells,
                k2_long.m_measured_cells, delta_cells, delta_cells * multiplier, k5_control.m_represented_cells,
                k2_long.m_represented_cells);
            std::fflush(stdout);
        }

        {
            Array<GpuParticle> patch;
            patch.reserve(kAffineOffsets.size());
            for (const auto &offset: kAffineOffsets) {
                auto row = restParticle();
                row.m_position_x = kAffineCentreX + offset[0u];
                row.m_position_y = kAffineCentreY + offset[1u];
                row.m_material = static_cast<float>(ESourceMaterial::liquid);
                row.m_mass = kAffineMass;
                row.m_volume = 1.0f;
                row.m_liquid_density = 1.0f;
                row.m_log_jp = 0.0f;
                row.m_displacement_x = static_cast<float>(kShearRate * static_cast<double>(offset[1u]));
                row.m_displacement_y = static_cast<float>(kShearRate * static_cast<double>(offset[0u]));
                row.m_deformation_displacement = {
                    0.0f, static_cast<float>(kShearRate), static_cast<float>(kShearRate), 0.0f};
                patch.push_back(row);
            }

            load(fixture, patch);
            const auto after_one = advance(fixture, kDefaultIterations, 1u);
            report_affine(kDefaultIterations, 1u, patch, after_one);

            load(fixture, patch);
            const auto after_long = advance(fixture, kDefaultIterations, kLongSubsteps);
            report_affine(kDefaultIterations, kLongSubsteps, patch, after_long);
        }

        // Structural acceptance only: every matrix cell, both horizons of the
        // schedule control and both affine horizons published. No motion magnitude
        // is asserted; the measured loss is reported as a limitation with numbers.
        constexpr u32 kMatrixCells = static_cast<u32>(
            kCoordinates.size() * kPhases.size() * kSpeeds.size() * kMasses.size());
        PPR_TEST_ASSERT(reported_cases == kMatrixCells * 2u + 4u);
        _.log(std::format("RESIDENT-PRECISION cases={} reports={}", kMatrixCells, reported_cases));
    };

    // ------------------------------------------------------------------
    // isolated liquid pressure recurrence diagnostic
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (isolated_liquid_density_error_decays_without_affine_growth) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        // One isolated sample has sum(W^2) < 1, so gathered reciprocal
        // volume exceeds one and the default volume blend remains inactive.
        // The density-error contract is the zero-gravity one, so the case
        // initializes that profile directly rather than rebuilding the solver.
        fixture.initialize(true, 256u, 256u, true, nullptr, 0.0f);

        const GpuParameters parameters = fixture.m_solver.profile();
        PPR_TEST_ASSERT(parameters.m_liquid_relaxation == 1.0f);
        PPR_TEST_ASSERT(parameters.m_gravity_strength == 0.0f);
        PPR_TEST_ASSERT(parameters.m_delta_time == 1.0f / 480.0f);
        PPR_TEST_ASSERT(parameters.m_fixed_point_multiplier == 10'000'000u);

        const std::array<std::pair<float, float>, 2u> cases{{{1.001f, 0.0f}, {0.999f, 0.00025f}}};
        for (const auto &[initial_density, initial_diagonal]: cases) {
            auto particle = restParticle();
            particle.m_position_x = 128.5f;
            particle.m_position_y = 128.5f;
            particle.m_material = static_cast<float>(ESourceMaterial::liquid);
            particle.m_mass = 1.0f;
            particle.m_volume = 1.0f;
            particle.m_liquid_density = initial_density;
            particle.m_deformation_displacement = {initial_diagonal, 0.0f, 0.0f, initial_diagonal};
            fixture.replace({&particle, 1u});

            for (u32 substep = 0u; substep <= 8u; ++substep) {
                if (substep != 0u) {
                    const u64 epoch = fixture.m_solver.accounting().m_epoch;
                    fixture.finishOneSubstep();
                    const auto snapshot = fixture.publishSnapshot(epoch);
                    PPR_TEST_ASSERT(snapshot.size() == 1u);
                    particle = snapshot[0u];
                }

                float max_D{};
                for (const float field: std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement))
                    max_D = std::max(max_D, std::abs(field));

                std::fprintf(
                    stderr,
                    "[isolated-liquid-pressure] initial_J=%.9g initial_D_diagonal=%.9g substep=%u K=2 J=%.9g trace_D=%.9g d=(%.9g,%.9g) max_abs_D=%.9g lambda=%.9g position=(%.9g,%.9g)\n",
                    static_cast<double>(initial_density), static_cast<double>(initial_diagonal),
                    static_cast<unsigned>(substep), static_cast<double>(particle.m_liquid_density),
                    static_cast<double>(
                        particle.m_deformation_displacement.m_c0x + particle.m_deformation_displacement.m_c1y),
                    static_cast<double>(particle.m_displacement_x), static_cast<double>(particle.m_displacement_y),
                    static_cast<double>(max_D), static_cast<double>(particle.m_lambda),
                    static_cast<double>(particle.m_position_x), static_cast<double>(particle.m_position_y));
                std::fflush(stderr);

                for (const float field: std::bit_cast<std::array<float, 24u>>(particle))
                    PPR_TEST_ASSERT(std::isfinite(field));

                PPR_TEST_ASSERT(std::abs(particle.m_liquid_density - 1.0f) < 0.0011f);
                PPR_TEST_ASSERT(max_D < 0.0011f);
                PPR_TEST_ASSERT(std::abs(particle.m_displacement_x) < 0.00001f);
                PPR_TEST_ASSERT(std::abs(particle.m_displacement_y) < 0.00001f);
                PPR_TEST_ASSERT(particle.m_mass == 1.0f and particle.m_volume == 1.0f);
                PPR_TEST_ASSERT(particle.m_material == static_cast<float>(ESourceMaterial::liquid));
                PPR_TEST_ASSERT(particle.m_enabled == 1.0f and particle.m_lambda == 0.0f);
                PPR_TEST_ASSERT(particle.m_position_x == 128.5f and particle.m_position_y == 128.5f);

                if (substep >= 2u) {
                    PPR_TEST_ASSERT(std::abs(particle.m_liquid_density - 1.0f) < 0.00001f);
                    PPR_TEST_ASSERT(max_D < 0.00001f);
                }
            }
        }
    };

    // ------------------------------------------------------------------
    // opt-in sand reference-occupancy diagnostics
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (sand_reference_occupancy_compression_and_floor_controls_diagnostic) {
        const char *const setting = std::getenv("PPR_PBMPM_COMPRESSION_DIAGNOSTIC");
        const std::string_view enabled = setting != nullptr ? setting : "0";
        PPR_TEST_ASSERT(enabled == "0" or enabled == "1");
        if (enabled != "1") {
            _.log(std::format("SAND-COMPRESSION-CONTROLS skipped: set PPR_PBMPM_COMPRESSION_DIAGNOSTIC=1"));
            return;
        }

        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, 64u, 64u, true, nullptr, 0.0f);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_use_grid_volume_for_liquid == 1u);
        const double h = 1.0 / 3.0;
        const double dt = static_cast<double>(fixture.m_solver.profile().m_delta_time);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_delta_time == 1.0f / 480.0f);
        // Explicit physical interpretation for this solver-only control:
        // basalt density2000, reference volume h*h*thickness1, cold mass2.
        const double physical_grain_mass = 2000.0 * h * h;
        const auto observe = [&](const std::string_view scene, const float phase, const u32 substep,
                                 const std::span<const GpuParticle> rows) {
            std::map<std::pair<int, int>, double> node_volume;
            Array<double> nearest(rows.size(), std::numeric_limits<double>::infinity());
            Array<double> plastic_logs;
            u64 pairs_quarter = 0u, pairs_half = 0u;
            for (std::size_t i = 0u; i < rows.size(); ++i) {
                for (std::size_t j = i + 1u; j < rows.size(); ++j) {
                    const double distance = std::hypot(
                        static_cast<double>(rows[i].m_position_x) - static_cast<double>(rows[j].m_position_x),
                        static_cast<double>(rows[i].m_position_y) - static_cast<double>(rows[j].m_position_y));
                    nearest[i] = std::min(nearest[i], distance);
                    nearest[j] = std::min(nearest[j], distance);
                    pairs_quarter += distance < 0.25 ? 1u : 0u;
                    pairs_half += distance < 0.5 ? 1u : 0u;
                }
            }

            double translational_ke = 0.0, apic_ke = 0.0;
            double momentum_x = 0.0, momentum_y = 0.0, com_x = 0.0, com_y = 0.0;
            double min_det = std::numeric_limits<double>::infinity(), max_det = -min_det;
            u32 zero_plastic_logs = 0u, negative_trace_count = 0u;
            double trace_min = std::numeric_limits<double>::infinity(), trace_max = -trace_min, trace_sum = 0.0;
            for (const auto &particle: rows) {
                for (const float value: std::bit_cast<std::array<float, 24u>>(particle)) {
                    PPR_TEST_ASSERT(std::isfinite(value));
                }

                PPR_TEST_ASSERT(particle.m_volume == 1.0f and particle.m_mass == 2.0f);
                PPR_TEST_ASSERT(particle.m_material == static_cast<float>(ESourceMaterial::sand));
                const float down_x = std::floor(particle.m_position_x);
                const float down_y = std::floor(particle.m_position_y);
                const float offset_x = (particle.m_position_x - down_x) - 0.5f;
                const float offset_y = (particle.m_position_y - down_y) - 0.5f;
                const std::array<float, 3u> wx{
                    0.5f * (0.5f - offset_x) * (0.5f - offset_x), 0.75f - offset_x * offset_x,
                    0.5f * (0.5f + offset_x) * (0.5f + offset_x)};
                const std::array<float, 3u> wy{
                    0.5f * (0.5f - offset_y) * (0.5f - offset_y), 0.75f - offset_y * offset_y,
                    0.5f * (0.5f + offset_y) * (0.5f + offset_y)};
                for (u32 y = 0u; y < 3u; ++y) {
                    for (u32 x = 0u; x < 3u; ++x) {
                        node_volume[{int(down_x) - 1 + int(x), int(down_y) - 1 + int(y)}] +=
                            static_cast<double>(particle.m_volume) * static_cast<double>(wx[x] * wy[y]);
                    }
                }

                const double vx = static_cast<double>(particle.m_displacement_x) * h / dt;
                const double vy = static_cast<double>(particle.m_displacement_y) * h / dt;
                const double kinetic = 0.5 * physical_grain_mass * (vx * vx + vy * vy);
                double affine_squared = 0.0;
                for (const float component: std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement)) {
                    const double value = static_cast<double>(component);
                    affine_squared += value * value;
                }

                const double trace = static_cast<double>(particle.m_deformation_displacement.m_c0x) +
                                     static_cast<double>(particle.m_deformation_displacement.m_c1y);
                trace_min = std::min(trace_min, trace);
                trace_max = std::max(trace_max, trace);
                trace_sum += trace;
                negative_trace_count += trace < 0.0 ? 1u : 0u;
                translational_ke += kinetic;
                apic_ke += kinetic + 0.5 * physical_grain_mass * h * h / (dt * dt) * 0.25 * affine_squared;
                momentum_x += physical_grain_mass * vx;
                momentum_y += physical_grain_mass * vy;
                com_x += (static_cast<double>(particle.m_position_x) - 3.0) * h;
                com_y += (static_cast<double>(particle.m_position_y) - 3.0) * h;
                plastic_logs.push_back(static_cast<double>(particle.m_log_jp));
                zero_plastic_logs += particle.m_log_jp == 0.0f ? 1u : 0u;
                const auto &f = particle.m_deformation_gradient;
                const double determinant = static_cast<double>(f.m_c0x) * static_cast<double>(f.m_c1y) -
                                           static_cast<double>(f.m_c1x) * static_cast<double>(f.m_c0y);
                min_det = std::min(min_det, determinant);
                max_det = std::max(max_det, determinant);
            }

            Array<double> occupancy, excess;
            double excess_sum = 0.0, excess_squared_sum = 0.0;
            for (const auto &[node, value]: node_volume) {
                static_cast<void>(node);
                occupancy.push_back(value);
                const double positive_excess = std::max(value - 1.0, 0.0);
                excess.push_back(positive_excess);
                excess_sum += positive_excess;
                excess_squared_sum += positive_excess * positive_excess;
            }

            std::ranges::sort(occupancy);
            std::ranges::sort(excess);
            std::ranges::sort(nearest);
            std::ranges::sort(plastic_logs);
            const auto p99 = static_cast<std::size_t>(std::ceil(0.99 * double(occupancy.size()))) - 1u;
            const double count = double(rows.size());
            if (scene == "isolated" or scene == "sparse_pair")
                PPR_TEST_ASSERT(occupancy.back() <= 1.0);

            _.log(
                std::format(
                    "SAND-COMPRESSION-CONTROL scene={} phase={} substep={} dt_s={} rows={} nodal_reference_V_max={} nodal_reference_V_p99={} positive_excess_max={} positive_excess_p99={} positive_excess_sum={} positive_excess_squared_sum={} nn_min_h={} nn_median_h={} pairs_lt_0_25h={} pairs_lt_0_5h={} translational_KE_J={} APIC_KE_J={} momentum_kg_m_s={},{} COM_m={},{} logJp_min={} logJp_median={} logJp_max={} logJp_exact_zero={} retainedD_trace_min={} retainedD_trace_mean={} retainedD_trace_max={} retainedD_trace_negative_fraction={} elastic_detF_min={} elastic_detF_max={} mass_kg={} cell_m={} cold_volume=1 cold_mass=2 CPU_reference_weights=true nodal_unit=source_cell_area_1 percentile=nearest_rank_over_stencil_nodes",
                    scene, phase, substep, dt, rows.size(), occupancy.back(), occupancy[p99], excess.back(),
                    excess[p99], excess_sum, excess_squared_sum, nearest.front(), nearest[(nearest.size() - 1u) / 2u],
                    pairs_quarter, pairs_half, translational_ke, apic_ke, momentum_x, momentum_y, com_x / count,
                    com_y / count, plastic_logs.front(), plastic_logs[(plastic_logs.size() - 1u) / 2u],
                    plastic_logs.back(), zero_plastic_logs, trace_min, trace_sum / count, trace_max,
                    double(negative_trace_count) / count, min_det, max_det, physical_grain_mass * count, h));
        };
        const auto run = [&](const std::string_view scene, const float phase, const Array<GpuParticle> &initial,
                             const TerrainView terrain = {}) {
            PPR_TEST_ASSERT(
                not fixture.m_solver.prepareReplacement(initial, {}, fixture.m_solver.accounting().m_epoch, terrain));
            fixture.m_solver.publishReplacement();
            PPR_TEST_ASSERT(fixture.m_solver.profile().m_use_grid_volume_for_liquid == 1u);
            _.log(
                std::format(
                    "SAND-COMPRESSION-SCENE scene={} phase={} gravity_strength={} volume_channel={} terrain={} source_floor_top=11.5 no_overlap_recovery=true",
                    scene, phase, fixture.m_solver.profile().m_gravity_strength,
                    fixture.m_solver.profile().m_use_grid_volume_for_liquid, not terrain.m_masks.empty()));
            observe(scene, phase, 0u, initial);
            const u32 end_substep = scene == "compressed_4x4" ? 128u : 64u;
            for (u32 substep = 1u; substep <= end_substep; ++substep) {
                fixture.finishOneSubstep();
                fixture.m_solver.publishTick();
                if (substep != 1u and substep != 8u and substep != 64u and substep != 128u)
                    continue;

                const auto rows = fixture.snapshot();
                PPR_TEST_ASSERT(rows.size() == initial.size());
                observe(scene, phase, substep, rows);
                if (scene == "isolated" or scene == "sparse_pair") {
                    for (std::size_t row = 0u; row < rows.size(); ++row) {
                        PPR_TEST_ASSERT(rows[row].m_position_x == initial[row].m_position_x);
                        PPR_TEST_ASSERT(rows[row].m_position_y == initial[row].m_position_y);
                        PPR_TEST_ASSERT(rows[row].m_displacement_x == 0.0f and rows[row].m_displacement_y == 0.0f);
                    }
                }
            }
        };

        // Deliberately compressed counterexamples, not changed production seeds.
        for (const float spacing: {0.75f, 1.0f}) {
            for (const float phase: {0.0f, 0.25f, 0.5f}) {
                Array<GpuParticle> particles;
                for (u32 y = 0u; y < 4u; ++y) {
                    for (u32 x = 0u; x < 4u; ++x) {
                        auto particle = restParticle();
                        particle.m_position_x = 28.0f + phase + float(x) * spacing;
                        particle.m_position_y = 28.0f + phase + float(y) * spacing;
                        particle.m_material = static_cast<float>(ESourceMaterial::sand);
                        particle.m_mass = 2.0f;
                        particle.m_log_jp = 0.0f;
                        particles.push_back(particle);
                    }
                }

                run(spacing == 0.75f ? "compressed_4x4" : "nominal_4x4", phase, particles);
            }
        }

        Array<GpuParticle> isolated(1u, restParticle());
        isolated[0u].m_position_x = 28.25f;
        isolated[0u].m_position_y = 28.25f;
        isolated[0u].m_material = static_cast<float>(ESourceMaterial::sand);
        isolated[0u].m_mass = 2.0f;
        isolated[0u].m_log_jp = 0.0f;
        run("isolated", 0.25f, isolated);
        auto sparse = isolated;
        sparse.push_back(isolated[0u]);
        sparse[1u].m_position_x += 3.0f;
        run("sparse_pair", 0.25f, sparse);

        const auto floor_masks = rectangleMasks(64u, 64u, 8u);
        const TerrainView floor{floor_masks, 64u, 64u, 3.0f, 3.0f};
        const auto make_strip = [&](const bool translating, const float height_h = 0.0f) {
            Array<GpuParticle> strip;
            for (u32 x = 0u; x < 16u; ++x) {
                auto particle = isolated[0u];
                particle.m_position_x = 24.5f + float(x);
                particle.m_position_y = 11.5f + height_h;
                particle.m_displacement_x = translating ? 0.01f : 0.0f;
                strip.push_back(particle);
            }

            return strip;
        };
        run("zero_gravity_tangent_terrain_strip", 0.5f, make_strip(true), floor);

        // Gravity supplies the actual normal impulse for terrain Coulomb tests;
        // the strip begins on the certified surface, without overlap correction.
        GpuParameters parameters = fixture.m_solver.profile();
        parameters.m_particle_count = 0u;
        parameters.m_shape_count = 0u;
        parameters.m_gravity_strength = 9.81f / (64.0f / 3.0f);
        PPR_TEST_ASSERT(not fixture.m_solver.shutdown());
        PPR_TEST_ASSERT(not fixture.m_solver.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            parameters));
        for (const bool translating: {false, true}) {
            run(translating ? "translating_gravity_terrain_strip" : "rest_gravity_terrain_strip", 0.5f,
                make_strip(translating), floor);
        }

        // A supported grid stencil can suspend centres above the surface.
        // Retain both hanging-layer heights as observations, without asserting
        // an unmeasured settling or Coulomb threshold in this diagnostic.
        for (const float height_h: {0.25f, 0.5f}) {
            for (const bool translating: {false, true}) {
                run(std::format("{}_gravity_terrain_strip_height_{}h", translating ? "translating" : "rest", height_h),
                    0.5f, make_strip(translating, height_h), floor);
            }
        }
    };

    PPR_UNIT_TEST (sand_objective_volume_relaxes_compression_without_attraction_and_preserves_restart) {
        Fixture fixture;
        GpuSolver disabled;
        PPR_DEFER {
            PPR_TEST_ASSERT(not disabled.shutdown());
            fixture.shutdown();
        };
        fixture.initialize(true, 64u, 64u, true, nullptr, 0.0f);
        GpuParameters parameters = fixture.m_solver.profile();
        parameters.m_use_grid_volume_for_liquid = 0u;
        PPR_TEST_ASSERT(not disabled.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            parameters));
        const auto patch = [](const float spacing, const float phase) {
            Array<GpuParticle> particles;
            for (u32 y = 0u; y < 4u; ++y) {
                for (u32 x = 0u; x < 4u; ++x) {
                    auto particle = restParticle();
                    particle.m_position_x = 28.0f + phase + float(x) * spacing;
                    particle.m_position_y = 28.0f + phase + float(y) * spacing;
                    particle.m_material = static_cast<float>(ESourceMaterial::sand);
                    particle.m_mass = 2.0f;
                    particle.m_log_jp = 0.0f;
                    particles.push_back(particle);
                }
            }

            return particles;
        };
        const auto capture = [](GpuSolver &solver) {
            const u64 epoch = solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not solver.beginSnapshot(epoch));
            PPR_TEST_ASSERT(not solver.drain());
            auto result = solver.pollSnapshot();
            PPR_TEST_ASSERT(result and result->has_value() and (**result).m_epoch == epoch);
            return std::move((**result).m_particles);
        };
        const auto evolve = [&](GpuSolver &solver, const std::span<const GpuParticle> initial, const u32 steps) {
            PPR_TEST_ASSERT(not solver.prepareReplacement(initial, {}, solver.accounting().m_epoch));
            solver.publishReplacement();
            for (u32 step = 0u; step < steps; ++step) {
                PPR_TEST_ASSERT(not solver.submitResidentTick(solver.accounting().m_epoch, 1u, 2u));
                PPR_TEST_ASSERT(not solver.drain());
                const auto ready = solver.pollTick();
                PPR_TEST_ASSERT(ready and *ready);
                solver.publishTick();
            }

            auto rows = capture(solver);
            PPR_TEST_ASSERT(rows.size() == initial.size());
            for (std::size_t row = 0u; row < rows.size(); ++row) {
                for (const float field: std::bit_cast<std::array<float, 24u>>(rows[row])) {
                    PPR_TEST_ASSERT(std::isfinite(field));
                }

                PPR_TEST_ASSERT(
                    rows[row].m_mass == initial[row].m_mass and rows[row].m_volume == initial[row].m_volume);
                PPR_TEST_ASSERT(rows[row].m_material == initial[row].m_material);
            }

            return rows;
        };
        const auto excess = [](const std::span<const GpuParticle> particles) {
            std::map<std::pair<int, int>, double> nodes;
            for (const auto &particle: particles) {
                const float down_x = std::floor(particle.m_position_x);
                const float down_y = std::floor(particle.m_position_y);
                const float ox = (particle.m_position_x - down_x) - 0.5f;
                const float oy = (particle.m_position_y - down_y) - 0.5f;
                const std::array<float, 3u> wx{
                    0.5f * (0.5f - ox) * (0.5f - ox), 0.75f - ox * ox, 0.5f * (0.5f + ox) * (0.5f + ox)};
                const std::array<float, 3u> wy{
                    0.5f * (0.5f - oy) * (0.5f - oy), 0.75f - oy * oy, 0.5f * (0.5f + oy) * (0.5f + oy)};
                for (u32 y = 0u; y < 3u; ++y) {
                    for (u32 x = 0u; x < 3u; ++x) {
                        nodes[{int(down_x) - 1 + int(x), int(down_y) - 1 + int(y)}] +=
                            static_cast<double>(particle.m_volume) * static_cast<double>(wx[x] * wy[y]);
                    }
                }
            }

            double total = 0.0;
            for (const auto &[node, volume]: nodes) {
                static_cast<void>(node);
                total += std::max(volume - 1.0, 0.0);
            }

            return total;
        };
        const auto same_physical_state = [](const std::span<const GpuParticle> first,
                                             const std::span<const GpuParticle> second) {
            PPR_TEST_ASSERT(first.size() == second.size());
            for (std::size_t row = 0u; row < first.size(); ++row) {
                const auto a = std::bit_cast<std::array<float, 24u>>(first[row]);
                const auto b = std::bit_cast<std::array<float, 24u>>(second[row]);
                // Position, translation, elastic F and retained affine D are the
                // physical response. Sand's legacy carried liquid J may differ.
                for (u32 field = 0u; field < 12u; ++field) {
                    PPR_TEST_ASSERT(a[field] == b[field]);
                }
            }
        };

        const auto same_semantic_state = [](const std::span<const GpuParticle> first,
                                             const std::span<const GpuParticle> second, const bool except_j) {
            PPR_TEST_ASSERT(first.size() == second.size());
            for (std::size_t row = 0u; row < first.size(); ++row) {
                const auto a = std::bit_cast<std::array<float, 24u>>(first[row]);
                const auto b = std::bit_cast<std::array<float, 24u>>(second[row]);
                for (u32 field = 0u; field < a.size(); ++field) {
                    if (except_j and field == 12u)
                        continue;

                    PPR_TEST_ASSERT(a[field] == b[field]);
                }
            }
        };

        for (const float phase: {0.0f, 0.25f, 0.5f}) {
            const auto initial = patch(0.75f, phase);
            const auto late = evolve(fixture.m_solver, initial, 128u);
            _.log(
                std::format(
                    "SAND-DENSITY-REGRESSION phase={} initial_excess_sum={} late128_excess_sum={}", phase,
                    excess(initial), excess(late)));
            PPR_TEST_ASSERT(excess(initial) > 0.0 and excess(late) == 0.0);
            const auto off = evolve(disabled, initial, 128u);
            same_physical_state(initial, off);

            const auto nominal = patch(1.0f, phase);
            same_physical_state(nominal, evolve(fixture.m_solver, nominal, 64u));
            const auto saved = evolve(fixture.m_solver, initial, 64u);
            const auto restarted = evolve(fixture.m_solver, saved, 64u);
            same_semantic_state(late, restarted, false);
            for (const float carried_j: {0.1f, 4.0f}) {
                auto alternative = initial;
                for (auto &particle: alternative) {
                    particle.m_liquid_density = carried_j;
                }

                same_semantic_state(late, evolve(fixture.m_solver, alternative, 128u), true);
            }

            // A consistent expansive velocity/affine field already separates
            // this compressed block. The unilateral channel must add no impulse.
            auto expanding = initial;
            const float centre = 28.0f + phase + 1.125f;
            for (auto &particle: expanding) {
                particle.m_displacement_x = 0.75f * (particle.m_position_x - centre);
                particle.m_displacement_y = 0.75f * (particle.m_position_y - centre);
                particle.m_deformation_displacement = {0.75f, 0.0f, 0.0f, 0.75f};
            }

            same_physical_state(evolve(disabled, expanding, 1u), evolve(fixture.m_solver, expanding, 1u));
        }

        auto isolated = patch(1.0f, 0.25f);
        isolated.resize(1u);
        PPR_TEST_ASSERT(excess(isolated) == 0.0);
        same_physical_state(isolated, evolve(fixture.m_solver, isolated, 64u));
        auto sparse = isolated;
        sparse.push_back(isolated[0u]);
        sparse[1u].m_position_x += 3.0f;
        PPR_TEST_ASSERT(excess(sparse) == 0.0);
        same_physical_state(sparse, evolve(fixture.m_solver, sparse, 64u));
    };

    // ------------------------------------------------------------------
    // shared-grid coupling distance control
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (shared_grid_coupling_precedes_support_contact_diagnostic) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        // Zero gravity, no terrain, no shapes and no brush: the only path
        // between the two rows is the quadratic node stencil. One resident
        // tick of one substep is one K2 pair, so the published row is the
        // first gather of a node state that both rows deposited into.
        fixture.initialize(true, 64u, 64u, true, nullptr, 0.0f);
        const GpuParameters parameters = fixture.m_solver.profile();
        PPR_TEST_ASSERT(parameters.m_gravity_strength == 0.0f);
        PPR_TEST_ASSERT(parameters.m_use_grid_volume_for_liquid == 1u);
        PPR_TEST_ASSERT(parameters.m_liquid_relaxation == 1.0f);

        // Requested V=.25 calibration. r is the equivalent-area radius and 2r
        // the drawn support; neither is a solver distance or an exclusion
        // radius, so this sweep measures the grid and not a collision circle.
        constexpr double kSampleVolume = 0.25;
        constexpr double kEquivalentRadius = 0.28209479177387814; // sqrt(.25/pi)
        constexpr double kDrawnSupport = 2.0 * kEquivalentRadius;
        constexpr double kSweepFirst = 0.5;
        constexpr double kSweepStep = 0.05;
        constexpr u32 kSweepSamples = 61u;
        constexpr double kSourceMomentum = 0.01;

        const auto sampleRow = [&](const double x, const double y, const double displacement_x) {
            GpuParticle row = restParticle();
            row.m_position_x = static_cast<float>(x);
            row.m_position_y = static_cast<float>(y);
            row.m_displacement_x = static_cast<float>(displacement_x);
            row.m_volume = static_cast<float>(kSampleVolume);
            row.m_material = static_cast<float>(ESourceMaterial::liquid);
            row.m_log_jp = 0.0f;
            return row;
        };

        // Every separation starts from a fresh replacement, so the pair is
        // rebuilt from the identical rest state and only the distance changes.
        const auto evolveOnce = [&](const std::span<const GpuParticle> rows) {
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(rows, {}, fixture.m_solver.accounting().m_epoch));
            fixture.m_solver.publishReplacement();
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(fixture.m_solver.accounting().m_epoch, 1u, 2u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());

            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
            fixture.m_solver.publishTick();

            return fixture.snapshot();
        };
        const auto identical = [](const GpuParticle &left, const GpuParticle &right) {
            return std::bit_cast<std::array<u32, 24u>>(left) == std::bit_cast<std::array<u32, 24u>>(right);
        };

        // A single undisturbed row is the disjoint-stencil baseline: at J=1
        // with zero displacement, gravity and affine motion it returns
        // bit-identical, so any pair difference is transferred state.
        const std::array<GpuParticle, 1u> control{sampleRow(29.0, 28.5, 0.0)};
        const auto control_rows = evolveOnce(control);
        PPR_TEST_ASSERT(control_rows.size() == 1u);
        PPR_TEST_ASSERT(identical(control_rows[0u], control[0u]));
        _.log("PBMPM-GRID-COUPLING baseline=isolated_single_row unchanged=true rows=1");

        // Fixed phase triple: between two node samples, just inside a cell,
        // and exactly on a node sample. The stencil phase, not the separation,
        // selects the shared nodes, so the onset is reported per phase.
        const std::array<double, 3u> anchors{28.0, 28.05, 28.5};
        const std::array<std::string_view, 3u> names{"between_nodes", "near_node", "on_node"};

        double widest_onset = 0.0;
        for (std::size_t phase = 0u; phase < anchors.size(); ++phase) {
            const double anchor = anchors[phase];
            double onset = 0.0;
            u32 coupled_samples = 0u;
            bool previous_coupled = false;
            for (u32 index = 0u; index < kSweepSamples; ++index) {
                const double separation = kSweepFirst + kSweepStep * static_cast<double>(index);
                const std::array<GpuParticle, 2u> pair{
                    sampleRow(anchor, 28.5, kSourceMomentum), sampleRow(anchor + separation, 28.5, 0.0)};

                const auto rows = evolveOnce(pair);
                PPR_TEST_ASSERT(rows.size() == pair.size());
                const bool coupled = not identical(rows[1u], pair[1u]);
                coupled_samples += coupled ? 1u : 0u;
                if (coupled)
                    onset = separation;

                // Requested separations: 0.5 cells is inside the grid reach and
                // still overlapping drawn supports (2R = 1.12837917); 1.5 is inside
                // the reach and disjoint by 0.37162083 cells; 3.0 and 3.5 lie beyond
                // every positive weight, so they must stay inert.
                if (index == 0u or std::abs(separation - 1.5) < 1e-9) {
                    PPR_TEST_ASSERT(coupled and rows[1u].m_displacement_x != 0.0f);
                } else if (std::abs(separation - 3.0) < 1e-9 or std::abs(separation - 3.5) < 1e-9) {
                    PPR_TEST_ASSERT(not coupled and rows[1u].m_displacement_x == 0.0f);
                }

                if (index == 0u or coupled != previous_coupled) {
                    _.log(
                        std::format(
                            "PBMPM-GRID-COUPLING phase={} separation_cells={:.4f} separation_over_r={:.3f} "
                            "separation_over_R={:.3f} coupled={} resting_displacement={:.9g}",
                            names[phase], separation, separation / kEquivalentRadius, separation / kDrawnSupport,
                            coupled, static_cast<double>(rows[1u].m_displacement_x)));
                }

                previous_coupled = coupled;
            }

            widest_onset = std::max(widest_onset, onset);
            _.log(
                std::format(
                    "PBMPM-GRID-COUPLING-SUMMARY phase={} anchor_cell={} sweep_first={} sweep_last={} step={} "
                    "samples={} coupled_samples={} onset_cells={:.4f} onset_over_r={:.3f} onset_over_R={:.3f} "
                    "disk_gap_cells={:.4f}",
                    names[phase], anchor, kSweepFirst,
                    kSweepFirst + kSweepStep * static_cast<double>(kSweepSamples - 1u), kSweepStep, kSweepSamples,
                    coupled_samples, onset, onset / kEquivalentRadius, onset / kDrawnSupport,
                    onset - 2.0 * kDrawnSupport));
        }

        // Observed model behaviour, not a desired contact law: the reach is
        // phase dependent, stops below three cells, and starts far beyond the
        // drawn supports. No assertion here claims a correct contact distance.
        PPR_TEST_ASSERT(widest_onset > 2.5);
        PPR_TEST_ASSERT(widest_onset < 3.0);
    };

    // ------------------------------------------------------------------
    // mean-occupancy blind spot census
    // ------------------------------------------------------------------

    struct OccupancyCensus {
        std::map<std::pair<int, int>, double> m_node_volume;
        Array<double> m_gathered_volume;
        u32 m_overfilled_nodes{};
    };

    std::array<double, 3u> quadraticAxisWeights(const double coordinate) noexcept {
        const double down = std::floor(coordinate);
        const double offset = (coordinate - down) - 0.5;
        return {0.5 * (0.5 - offset) * (0.5 - offset), 0.75 - offset * offset, 0.5 * (0.5 + offset) * (0.5 + offset)};
    }

    /// Reproduces the two quantities the objective-volume gate consumes: the
    /// deposited node volume (all rows) and each row's own gathered volume
    /// (one row). Double precision is the census, not the device arithmetic.
    OccupancyCensus depositCensus(const std::span<const GpuParticle> rows) {
        OccupancyCensus census;
        census.m_gathered_volume.resize(rows.size());

        for (const GpuParticle &row: rows) {
            const double x = static_cast<double>(row.m_position_x);
            const double y = static_cast<double>(row.m_position_y);
            const auto wx = quadraticAxisWeights(x);
            const auto wy = quadraticAxisWeights(y);
            const int base_x = static_cast<int>(std::floor(x)) - 1;
            const int base_y = static_cast<int>(std::floor(y)) - 1;

            for (u32 vertical = 0u; vertical < 3u; ++vertical) {
                for (u32 horizontal = 0u; horizontal < 3u; ++horizontal) {
                    census
                        .m_node_volume[{base_x + static_cast<int>(horizontal), base_y + static_cast<int>(vertical)}] +=
                        static_cast<double>(row.m_volume) * wx[horizontal] * wy[vertical];
                }
            }
        }

        // Second pass: the gate reads each row's gathered node volume, not the
        // peak node, over the same weights and the same 3x3 stencil.
        for (std::size_t index = 0u; index < rows.size(); ++index) {
            const double x = static_cast<double>(rows[index].m_position_x);
            const double y = static_cast<double>(rows[index].m_position_y);
            const auto wx = quadraticAxisWeights(x);
            const auto wy = quadraticAxisWeights(y);
            const int base_x = static_cast<int>(std::floor(x)) - 1;
            const int base_y = static_cast<int>(std::floor(y)) - 1;

            double gathered = 0.0;
            for (u32 vertical = 0u; vertical < 3u; ++vertical) {
                for (u32 horizontal = 0u; horizontal < 3u; ++horizontal) {
                    gathered += wx[horizontal] *
                                wy[vertical] *
                                census.m_node_volume.at(
                                    {base_x + static_cast<int>(horizontal), base_y + static_cast<int>(vertical)});
                }
            }

            census.m_gathered_volume[index] = gathered;
        }

        for (const auto &[node, volume]: census.m_node_volume) {
            static_cast<void>(node);
            census.m_overfilled_nodes += volume > 1.0 ? 1u : 0u;
        }

        return census;
    }

    PPR_UNIT_TEST (mean_occupancy_gate_blind_spot_census_diagnostic) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        // Zero gravity and Sand identity leave the objective-volume impulse as
        // the only responder. The volume flag keeps its production default.
        fixture.initialize(true, 64u, 64u, true, nullptr, 0.0f);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_use_grid_volume_for_liquid == 1u);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_gravity_strength == 0.0f);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_liquid_relaxation == 1.0f);

        constexpr double kSampleVolume = 0.25;
        constexpr u32 kSamplesPerCell = 4u;
        // Node-centred quadratic products per axis: 0.75*0.75 on the centre
        // node, and a squared weight sum of (0.125^2 + 0.75^2 + 0.125^2)^2.
        constexpr double kCentreNodeProduct = 0.5625;
        constexpr double kStencilProductSum = 0.3525390625;

        const auto cluster = [&](const u32 count) {
            Array<GpuParticle> rows;
            for (u32 index = 0u; index < count; ++index) {
                GpuParticle row = restParticle();
                row.m_position_x = 28.5f;
                row.m_position_y = 28.5f;
                row.m_volume = static_cast<float>(kSampleVolume);
                row.m_material = static_cast<float>(ESourceMaterial::sand);
                // Zero plastic history isolates the objective-volume channel
                // from the inherited Sand logJp decay.
                row.m_log_jp = 0.0f;
                rows.push_back(row);
            }

            return rows;
        };
        const auto evolve = [&](const std::span<const GpuParticle> initial, const u32 steps) {
            PPR_TEST_ASSERT(
                not fixture.m_solver.prepareReplacement(initial, {}, fixture.m_solver.accounting().m_epoch));
            fixture.m_solver.publishReplacement();
            for (u32 step = 0u; step < steps; ++step) {
                PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(fixture.m_solver.accounting().m_epoch, 1u, 2u));
                PPR_TEST_ASSERT(not fixture.m_solver.drain());

                const auto complete = fixture.m_solver.pollTick();
                PPR_TEST_ASSERT(complete and *complete);
                fixture.m_solver.publishTick();
            }

            return fixture.snapshot();
        };
        const auto trace = [](const GpuParticle &row) {
            return static_cast<double>(row.m_deformation_displacement.m_c0x) +
                   static_cast<double>(row.m_deformation_displacement.m_c1y);
        };
        const auto coincident = [](const std::span<const GpuParticle> rows) {
            for (std::size_t left = 0u; left < rows.size(); ++left) {
                for (std::size_t right = left + 1u; right < rows.size(); ++right) {
                    if (rows[left].m_position_x != rows[right].m_position_x or
                        rows[left].m_position_y != rows[right].m_position_y)
                        return false;
                }
            }

            return true;
        };

        // The audit's analytic control: eleven coincident V=.25 samples
        // overfill the centre node while every sample's own gathered volume
        // stays below one, and twelve crosses the gate. Coincidence is the
        // extreme deterministic case, not a produced seed; it isolates the
        // gate algebra from worldgen and from the material inventory.
        for (const u32 count: {11u, 12u}) {
            const auto initial = cluster(count);
            const OccupancyCensus census = depositCensus(initial);
            double peak_node = 0.0;
            for (const auto &[node, volume]: census.m_node_volume) {
                static_cast<void>(node);
                peak_node = std::max(peak_node, volume);
            }

            double peak_gathered = 0.0;
            for (const double gathered: census.m_gathered_volume)
                peak_gathered = std::max(peak_gathered, gathered);

            const double expected_node = static_cast<double>(count) * kSampleVolume * kCentreNodeProduct;
            const double expected_gathered = static_cast<double>(count) * kSampleVolume * kStencilProductSum;
            const u32 underfilled_nodes = static_cast<u32>(census.m_node_volume.size()) - census.m_overfilled_nodes;

            PPR_TEST_ASSERT(std::abs(peak_node - expected_node) < 1e-9);
            PPR_TEST_ASSERT(std::abs(peak_gathered - expected_gathered) < 1e-9);
            PPR_TEST_ASSERT(census.m_gathered_volume.size() == initial.size());
            PPR_TEST_ASSERT(census.m_overfilled_nodes == 1u and underfilled_nodes == 8u);
            PPR_TEST_ASSERT(peak_node > 1.0);
            PPR_TEST_ASSERT(count > kSamplesPerCell);

            const bool gate_open = peak_gathered > 1.0;
            const auto first = evolve(initial, 1u);
            PPR_TEST_ASSERT(first.size() == initial.size());
            if (gate_open) {
                // The open gate adds one isotropic impulse whose trace is twice
                // the retained diagonal; it is a mean/affine term, not a pair
                // force, so the rows stay exactly coincident.
                PPR_TEST_ASSERT(trace(first.front()) > 0.0057 and trace(first.front()) < 0.0058);
            } else {
                for (std::size_t index = 0u; index < first.size(); ++index) {
                    PPR_TEST_ASSERT(trace(first[index]) == 0.0);
                    PPR_TEST_ASSERT(first[index].m_displacement_x == 0.0f);
                    PPR_TEST_ASSERT(first[index].m_position_x == initial[index].m_position_x);
                }
            }

            // The fixed eight-substep tick budget cannot separate either case:
            // the closed gate never responds, and the open gate responds at
            // the shared mean, which leaves every row at the same position.
            const auto late = evolve(initial, 8u);
            PPR_TEST_ASSERT(late.size() == initial.size());
            PPR_TEST_ASSERT(coincident(late));
            for (std::size_t index = 0u; index < late.size(); ++index)
                PPR_TEST_ASSERT(std::isfinite(trace(late[index])));

            if (not gate_open) {
                for (std::size_t index = 0u; index < late.size(); ++index) {
                    PPR_TEST_ASSERT(trace(late[index]) == 0.0);
                    PPR_TEST_ASSERT(late[index].m_log_jp == 0.0f);
                    PPR_TEST_ASSERT(late[index].m_position_x == initial[index].m_position_x);
                    PPR_TEST_ASSERT(late[index].m_position_y == initial[index].m_position_y);
                }
            }

            _.log(
                std::format(
                    "PBMPM-OCCUPANCY-CENSUS rows={} samples_per_cell_capacity={} centre_node_volume={:.9f} "
                    "peak_node_volume={:.9f} peak_gathered_volume={:.9f} overfilled_nodes={} underfilled_nodes={} "
                    "gate_open={} trace_D_after_1={:.9g} trace_D_after_8={:.9g} coincident_after_8={}",
                    count, kSamplesPerCell, expected_node, peak_node, peak_gathered, census.m_overfilled_nodes,
                    underfilled_nodes, gate_open, trace(first.front()), trace(late.front()), coincident(late)));
        }
    };

    // ------------------------------------------------------------------
    // sand terrain Coulomb response and unchanged base-material contacts
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (sand_terrain_friction_is_dissipative_and_preserves_body_and_guardian_order) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, 32u, 32u, true, nullptr, 0.0f);
        // The existing probe's explicit integer input has one unit of mass and
        // 1e-7 displacement quantization. Eight units also cover normal arithmetic.
        constexpr float tolerance = 8.0f / 10'000'000.0f;
        const auto probe = [&](const std::string_view name, const TerrainView terrain, const float dx, const float dy,
                               const std::span<const GpuBodyBoundary> bodies = {}) {
            const auto result = contactProbe(
                fixture, terrain, bodies, dy, {}, 10'000'000, dx, {}, {}, {}, false, false, false, false, true);
            PPR_TEST_ASSERT(result.m_errors[0u] == 0u);
            std::array<float, 12u> values;
            for (u32 field = 0u; field < values.size(); ++field) {
                values[field] = std::bit_cast<float>(result.m_contacts[field]);
                PPR_TEST_ASSERT(std::isfinite(values[field]));
            }

            // Activating sand's separate velocity must leave the existing base
            // result bit-identical, including liquid and unsupported inventory.
            for (u32 field = 0u; field < 4u; ++field) {
                PPR_TEST_ASSERT(result.m_contacts[field] == result.m_contacts[6u + field]);
            }

            PPR_TEST_ASSERT(result.m_contacts[0u] == result.m_contacts[10u]);
            PPR_TEST_ASSERT(result.m_contacts[1u] == result.m_contacts[11u]);
            _.log(
                std::format(
                    "SAND-TERRAIN-FRICTION case={} input={},{} base={},{} sand={},{} base_mass={} flags={}", name, dx,
                    dy, values[0u], values[1u], values[4u], values[5u], values[2u], result.m_errors[0u]));
            return values;
        };
        const auto near = [&](const float actual, const float expected) {
            PPR_TEST_ASSERT(std::abs(actual - expected) <= tolerance);
        };
        const auto dissipative = [&](const std::array<float, 12u> &values) {
            const float base_energy = values[0u] * values[0u] + values[1u] * values[1u];
            const float sand_energy = values[4u] * values[4u] + values[5u] * values[5u];
            PPR_TEST_ASSERT(sand_energy <= base_energy + tolerance);
            PPR_TEST_ASSERT(values[0u] * values[4u] + values[1u] * values[5u] >= -tolerance);
        };

        // At node centre(6.5,7.5), the authored horizontal contour is exactly
        // y=7.5. The normal impulse is real inward motion, never recovery depth.
        const auto flat_masks = rectangleMasks(8u, 4u, 2u);
        const TerrainView flat{flat_masks, 8u, 4u, 3.5f, 5.0f};
        const auto sliding = probe("flat_sliding", flat, 0.5f, -0.25f);
        near(sliding[0u], 0.5f);
        near(sliding[1u], 0.0f);
        near(sliding[4u], 0.375f);
        near(sliding[5u], 0.0f);
        dissipative(sliding);
        const auto stopping = probe("flat_stop_without_reversal", flat, 0.25f, -0.5f);
        near(stopping[4u], 0.0f);
        near(stopping[5u], 0.0f);
        dissipative(stopping);
        const auto reverse_direction = probe("flat_negative_tangent", flat, -0.5f, -0.25f);
        near(reverse_direction[4u], -0.375f);
        near(reverse_direction[5u], 0.0f);
        dissipative(reverse_direction);
        const auto tangent = probe("flat_tangent_without_normal_impulse", flat, 0.25f, 0.0f);
        near(tangent[4u], 0.25f);
        near(tangent[5u], 0.0f);
        const auto resting = probe("flat_rest", flat, 0.0f, 0.0f);
        near(resting[4u], 0.0f);
        near(resting[5u], 0.0f);
        const auto absent = probe("no_terrain", {}, 0.5f, -0.25f);
        near(absent[4u], 0.5f);
        near(absent[5u], -0.25f);

        // Canonical mask1 has x+y=.5 as its exposed 45-degree face. Its
        // translated midpoint is the same node centre as the flat case.
        const std::array<u32, 1u> diagonal_mask{1u};
        const TerrainView diagonal{diagonal_mask, 1u, 1u, 6.25f, 7.25f};
        const auto slope = probe("diagonal_sliding", diagonal, 0.05f, -0.15f);
        near(slope[0u], 0.1f);
        near(slope[1u], -0.1f);
        near(slope[4u], 0.075f);
        near(slope[5u], -0.075f);
        dissipative(slope);

        // Preserve the source body's actual inside-query point semantics. The
        // base endpoint6.5 lies inside its left half and becomes zero carry;
        // the already-frictional sand endpoint6.375 is outside left face6.4.
        // Applying friction after bodies would incorrectly erase sand's tangent.
        GpuBodyBoundary body{};
        body.m_shape.m_position_x = 6.6f;
        body.m_shape.m_position_y = 7.0f;
        body.m_shape.m_half_size_x = 0.2f;
        body.m_shape.m_half_size_y = 1.0f;
        body.m_com_x = 6.6f;
        body.m_com_y = 7.0f;
        const auto ordered = probe("flat_then_static_body", flat, 0.5f, -0.25f, {&body, 1u});
        near(ordered[0u], 0.0f);
        near(ordered[1u], 0.0f);
        near(ordered[4u], 0.375f);
        near(ordered[5u], 0.0f);

        // Both diagonal endpoints lie in the body's left half, at different
        // contact heights. The probe uses dt=.125, so omega=.8 gives omega*dt=.1.
        // Each candidate must query its own rotational normal carry; reusing
        // the base contact point would incorrectly give sand an x carry of .01.
        GpuBodyBoundary rotating{};
        rotating.m_shape.m_position_x = 6.11f;
        rotating.m_shape.m_position_y = 7.0f;
        rotating.m_shape.m_half_size_x = 0.05f;
        rotating.m_shape.m_half_size_y = 1.0f;
        rotating.m_com_x = 6.11f;
        rotating.m_com_y = 7.0f;
        rotating.m_angular_velocity = 0.8f;
        const auto rotating_ordered = probe("diagonal_then_rotating_body", diagonal, 0.05f, -0.15f, {&rotating, 1u});
        near(rotating_ordered[0u], 0.01f);
        near(rotating_ordered[1u], -0.1f);
        near(rotating_ordered[4u], 0.0075f);
        near(rotating_ordered[5u], -0.075f);

        // A wider floor keeps this large inward/tangential endpoint on real
        // terrain before the guardian clips node6-3 below its padding4.
        const auto wide_masks = rectangleMasks(32u, 4u, 2u);
        const TerrainView wide{wide_masks, 32u, 4u, 0.0f, 5.0f};
        const auto guarded = probe("terrain_then_guardian", wide, -3.0f, -0.25f);
        near(guarded[0u], 0.0f);
        near(guarded[1u], 0.0f);
        near(guarded[4u], 0.0f);
        near(guarded[5u], 0.0f);
    };

    // ------------------------------------------------------------------
    // resident publication and render observations
    // ------------------------------------------------------------------

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
    // frozen production-solver liquid presentation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (evolved_liquid_pool_has_no_interior_render_holes) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, 64u, 64u, true, nullptr, 9.81f / (64.0f / 3.0f));

        // Three solid boxes enclose x=[12,36], with the floor's upper face at
        // y=12. Source coordinates and world coordinates both have +Y up.
        const std::array<GpuStaticShape, 3u> walls{{
            {.m_position_x = 24.0f, .m_position_y = 11.0f, .m_half_size_x = 13.0f, .m_half_size_y = 1.0f},
            {.m_position_x = 11.0f, .m_position_y = 28.0f, .m_half_size_x = 1.0f, .m_half_size_y = 16.0f},
            {.m_position_x = 37.0f, .m_position_y = 28.0f, .m_half_size_x = 1.0f, .m_half_size_y = 16.0f},
        }};
        // Match production seeding: one row per fine cell, source spacing and
        // cold volume both 1 (physical spacing 1/3m, volume 1/9m3).
        Array<GpuParticle> initial;
        for (u32 y = 0u; y < 15u; ++y) {
            for (u32 x = 0u; x < 24u; ++x) {
                GpuParticle particle{};
                particle.m_position_x = 12.0f + float(x) + 0.5f;
                particle.m_position_y = 12.0f + float(y) + 0.5f;
                particle.m_volume = particle.m_mass = 1.0f;
                particle.m_material = static_cast<float>(ESourceMaterial::liquid);
                initial.push_back(particle);
            }
        }

        fixture.replace(initial, walls);
        std::array<Array<GpuParticle>, 2u> frozen;
        std::array<u64, 2u> epochs{};
        for (u32 tick = 1u; tick <= 120u; ++tick) {
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(fixture.m_solver.accounting().m_epoch, 8u, 2u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            const auto ready = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(ready and *ready);
            fixture.m_solver.publishTick();
            if (tick == 1u or tick == 120u) {
                const u32 index = tick == 1u ? 0u : 1u;
                epochs[index] = fixture.m_solver.accounting().m_epoch;
                frozen[index] = fixture.snapshot();
                PPR_TEST_ASSERT(frozen[index].size() == initial.size());
            }
        }

        Renderer renderer;
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        PPR_DEFER {
            PPR_TEST_ASSERT(not renderer.shutdown());
        };
        auto &device = fixture.m_rhi->getDevice();
        rhi::TextureDesc desc{};
        desc.type = rhi::TextureType::Texture2D;
        desc.size = {512u, 512u, 1u};
        desc.format = rhi::Format::RGBA8Unorm;
        desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
        desc.defaultState = rhi::ResourceState::RenderTarget;
        rhi::ComPtr<rhi::ITexture> target;
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, target.writeRef())));
        const auto viewport = rhi::Viewport::fromSize(512.0f, 512.0f);
        const auto scissor = rhi::ScissorRect::fromSize(512u, 512u);
        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_ortho_scale = 12.0f / 512.0f;
        model.m_origin = float3{2.0f, 2.0f, -0.5f};
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 512, 512}});
        const float3 centre{8.0f, 8.0f, -0.5f};
        model.m_origin = centre - camera.getRight() * 6.0f - camera.getUp() * 6.0f;
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 512, 512}});
        auto snapshot = camera.getSnapshot();
        snapshot.m_has_camera_cut = false;
        const char *const qa = std::getenv("PPR_GRID_QA_DIR");
        const char *const content_root = std::getenv("PPR_GRID_QA_CONTENT_ROOT");
        const bool glossy_qa = qa != nullptr and *qa != '\0' and content_root != nullptr and *content_root != '\0';
        Array<image::ImageAsset> backdrop;
        if (glossy_qa) {
            for (const char *const name: {"distant", "caverns", "formations", "backwall"}) {
                const auto path = std::filesystem::path{content_root} /
                                  "textures" /
                                  std::format("background_frontier_{}.png", name);
                const auto mapped = mem::SharedBuffer::mapFile(path);
                PPR_TEST_ASSERT(mapped.has_value());
                auto decoded = image::decodeToRgba8(
                    mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
                PPR_TEST_ASSERT(decoded.has_value());
                PPR_TEST_ASSERT(not image::generateMipChain(*decoded, image::MipGenDesc{}));
                backdrop.push_back(std::move(*decoded));
            }
        }

        std::array<u64, 2u> holes{};
        std::array<u64, 2u> samples{};
        // Painted pixels in cells that nominated no particle of their own, keyed
        // by the cell-space Chebyshev distance to the nearest cell that did: band
        // 1, band 2, band 3 and band 4-or-more. The reconstruction's influence
        // radius decides which of those bands may legitimately receive paint.
        constexpr u32 kPoolBands = 4u;
        std::array<std::array<u64, kPoolBands>, 2u> outside_band_painted{};
        std::array<u64, 2u> outside_edge_band_painted{};
        std::array<u64, 2u> far_field_painted{};
        std::array<u64, 2u> precedence_samples{};
        std::array<u64, 2u> precedence_ok{};
        std::array<u64, 2u> precedence_edge_band_samples{};
        std::array<u64, 2u> water_pixels{};
        // A one-pixel band around each cell boundary is excluded from the strict
        // interior judgement. The presentation reads a screen-space field that two
        // separable Gaussian passes have already smoothed, so a pixel whose
        // footprint straddles two cells is attributable to neither. The fixture
        // maps 12 m onto 512 px and one native cell is 1/3 m, so a cell is
        // 512/(12*3) = 14.2222 px and the band is 1/14.2222 = 0.0703125 cell per
        // side. Band paint is reported separately and never used to declare the
        // pool gate passed.
        constexpr double kPoolTargetPixels = 512.0;
        constexpr double kPoolWorldMetres = 12.0;
        constexpr double kPoolNativeCellMetres = 1.0 / 3.0;
        constexpr double kPoolEdgeBandPixels = 1.0;
        const double pool_cell_pixels = kPoolTargetPixels * kPoolNativeCellMetres / kPoolWorldMetres;
        const double pool_edge_band = kPoolEdgeBandPixels / pool_cell_pixels;
        for (u32 state = 0u; state < 2u; ++state) {
            u64 hash = 14695981039346656037ull;
            double rms_motion_squared = 0.0;
            for (const auto &particle: frozen[state]) {
                for (const auto byte: std::as_bytes(std::span{&particle, 1u})) {
                    hash = (hash ^ std::to_integer<u8>(byte)) * 1099511628211ull;
                }

                const double dx = static_cast<double>(particle.m_displacement_x);
                const double dy = static_cast<double>(particle.m_displacement_y);
                rms_motion_squared += dx * dx + dy * dy;
            }

            if (qa != nullptr and *qa != '\0') {
                std::filesystem::create_directories(qa);
                std::ofstream output{
                    std::filesystem::path{qa} / std::format("solver-pool-epoch-{}.gpu-rows.bin", epochs[state]),
                    std::ios::binary};
                output.write(
                    reinterpret_cast<const char *>(frozen[state].data()),
                    std::streamsize(frozen[state].size() * sizeof(GpuParticle)));
                PPR_TEST_ASSERT(output.good());
            }

            // Approved categorical presentation: project the frozen solver rows
            // into native cells with the same ordered key the production
            // projection uses (nearest cell centre, stable-ID tie). The same rows
            // also nominate the committed winner grid, so a winning cell is
            // particle-owned and retires the categorical base under it, while the
            // one injected terrain cell keeps no winner: terrain owns it and
            // excludes the particle density field there.
            constexpr u32 kPoolEdge = WorldLightingPass::kWorldEdge;
            constexpr u32 kPrecedenceX = 21u;
            constexpr u32 kPrecedenceY = 15u;
            std::vector<u32> resolved(WorldLightingPass::kCellCount, 0u);
            std::vector<u32> best_key(WorldLightingPass::kCellCount, 0xffffffffu);
            std::vector<u32> best_row(WorldLightingPass::kCellCount, 0xffffffffu);
            resolved[kPrecedenceY * kPoolEdge + kPrecedenceX] = *lighting::packCell(2u, 300.0);
            for (u32 row = 0u; row < frozen[state].size(); ++row) {
                const auto &particle = frozen[state][row];
                const float native_x = particle.m_position_x - 3.0f;
                const float native_y = particle.m_position_y - 3.0f;
                if (not std::isfinite(native_x) or
                    not std::isfinite(native_y) or
                    native_x < 0.0f or
                    native_y < 0.0f or
                    native_x > static_cast<float>(kPoolEdge) or
                    native_y > static_cast<float>(kPoolEdge)) {
                    continue;
                }

                const auto axis = [](const float native) {
                    return std::min<u32>(
                        static_cast<u32>(std::floor(std::min(native, static_cast<float>(kPoolEdge)))), kPoolEdge - 1u);
                };
                const u32 cell_x = axis(native_x);
                const u32 cell_y = axis(native_y);
                const u32 cell = cell_y * kPoolEdge + cell_x;
                if (resolved[cell] != 0u) {
                    continue;
                }

                const float delta_x = native_x - (static_cast<float>(cell_x) + 0.5f);
                const float delta_y = native_y - (static_cast<float>(cell_y) + 0.5f);
                float distance2 = delta_x * delta_x + delta_y * delta_y;
                if (distance2 == 0.0f) {
                    distance2 = 0.0f;
                }

                const u32 key = std::bit_cast<u32>(distance2);
                if (key < best_key[cell] or (key == best_key[cell] and row < best_row[cell])) {
                    best_key[cell] = key;
                    best_row[cell] = row;
                }
            }
            for (u32 cell = 0u; cell < resolved.size(); ++cell) {
                if (resolved[cell] == 0u and best_row[cell] != 0xffffffffu) {
                    resolved[cell] = *lighting::packCell(1u, 300.0);
                }
            }

            // The committed winner grid is published with the resolved cells: the
            // nominating row where this model recorded a winner, the no-winner
            // sentinel everywhere else. The injected terrain cell therefore keeps
            // the sentinel and stays terrain-owned, which is what excludes the
            // particle density over it.
            std::vector<u32> ownership(WorldLightingPass::kCellCount, 0xffffffffu);
            for (u32 cell = 0u; cell < ownership.size(); ++cell) {
                if (best_row[cell] != 0xffffffffu) {
                    ownership[cell] = best_row[cell];
                }
            }

            // Cell-space Chebyshev distance from every cell to the nearest cell
            // that nominated a particle, saturated where nothing in the grid is
            // nominated. One multi-source breadth-first expansion over the
            // 8-neighbourhood settles each cell once, so the pass is linear in the
            // cell count and the distance is exact; no pairwise cell scan is
            // needed to bound how far a neighbour's paint may reach.
            constexpr u32 kPoolUnreached = 0xffffffffu;
            std::vector<u32> nomination_distance(WorldLightingPass::kCellCount, kPoolUnreached);
            std::vector<u32> frontier;
            frontier.reserve(WorldLightingPass::kCellCount);
            for (u32 cell = 0u; cell < nomination_distance.size(); ++cell) {
                if (best_row[cell] != 0xffffffffu) {
                    nomination_distance[cell] = 0u;
                    frontier.push_back(cell);
                }
            }

            for (std::size_t head = 0u; head < frontier.size(); ++head) {
                const u32 cell = frontier[head];
                const u32 next_distance = nomination_distance[cell] + 1u;
                const u32 cell_x = cell % kPoolEdge;
                const u32 cell_y = cell / kPoolEdge;

                for (i32 step_y = -1; step_y <= 1; ++step_y) {
                    for (i32 step_x = -1; step_x <= 1; ++step_x) {
                        const i32 neighbour_x = static_cast<i32>(cell_x) + step_x;
                        const i32 neighbour_y = static_cast<i32>(cell_y) + step_y;
                        const bool inside = neighbour_x >= 0 and
                                            neighbour_y >= 0 and
                                            neighbour_x < static_cast<i32>(kPoolEdge) and
                                            neighbour_y < static_cast<i32>(kPoolEdge);
                        if (not inside) {
                            continue;
                        }

                        const u32 neighbour = static_cast<u32>(neighbour_y) * kPoolEdge + static_cast<u32>(neighbour_x);
                        if (nomination_distance[neighbour] != kPoolUnreached) {
                            continue;
                        }

                        nomination_distance[neighbour] = next_distance;
                        frontier.push_back(neighbour);
                    }
                }
            }

            for (u32 variant = 0u; variant < (glossy_qa ? 2u : 1u); ++variant) {
                const bool glossy = variant != 0u;
                {
                    GridPass grid;
                    PPR_TEST_ASSERT(
                        not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
                    const auto shader_buffer = [&](const u64 bytes, const u32 stride, const void *const data) {
                        rhi::BufferDesc desc{};
                        desc.size = bytes;
                        desc.elementSize = stride;
                        desc.usage = static_cast<rhi::BufferUsage>(
                            static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                            static_cast<u32>(rhi::BufferUsage::CopySource));
                        desc.defaultState = rhi::ResourceState::ShaderResource;
                        rhi::ComPtr<rhi::IBuffer> result;
                        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, data, result.writeRef())));
                        return result;
                    };

                    // A constant fine-CH field (DC = 1/Y0) makes the categorical
                    // material response unit-lit; fine-CH fitting itself is
                    // covered by the world-lighting fine tests.
                    std::vector<lighting::PackedCH> fine_ch(lighting::kFineCHRecordCount);
                    lighting::CH unit{};
                    unit.m_coefficients[0] = float3{2.5066282746f, 2.5066282746f, 2.5066282746f};
                    const auto packed_unit = lighting::packCH(unit);
                    PPR_TEST_ASSERT(packed_unit.has_value());
                    std::fill(fine_ch.begin(), fine_ch.end(), *packed_unit);
                    PPR_TEST_ASSERT(not grid.setLightingBuffersForTest(
                        shader_buffer(resolved.size() * sizeof(u32), sizeof(u32), resolved.data()),
                        shader_buffer(
                            fine_ch.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH), fine_ch.data())));
                    PPR_TEST_ASSERT(not grid.setParticleOwnershipForTest(
                        shader_buffer(ownership.size() * sizeof(u32), sizeof(u32), ownership.data())));
                    PPR_DEFER {
                        const auto submitted = renderer.lastSubmittedFenceValue();
                        if (submitted != kFenceValueNone) {
                            PPR_TEST_ASSERT(not renderer.waitForFenceValue(submitted));
                        }

                        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
                        PPR_TEST_ASSERT(not grid.shutdown());
                    };

                    GridPass::GridStagedFrame stage{};
                    stage.m_cell_size_m = 1.0f / 3.0f;
                    GridTerrainVisual water{};
                    water.m_material_id = 1u;
                    water.m_color_rgba = 0x0000ffffu;
                    water.m_ior = 1.0f;
                    water.m_surface_flags = kGridSurfaceParticleLiquid;
                    if (glossy) {
                        water.m_color_rgba = 0x3989c970u;
                        water.m_roughness = 0.12f;
                        water.m_ior = 1.333f;
                        water.m_normal_scale = 0.8f;
                        water.m_refraction_depth_m = 0.2f;
                        water.m_emissive_strength = 1.0f;
                        water.m_opacity = 1.0f;
                        water.m_surface_flags = kGridSurfaceParticleLiquid | kGridSurfaceOpticalBlend;
                        stage.m_has_background = true;
                        stage.m_terrain_images = backdrop;
                        constexpr std::array<float, 4u> periods{256.0f, 180.0f, 112.0f, 64.0f};
                        constexpr std::array<float, 4u> parallax{0.08f, 0.22f, 0.45f, 0.7f};
                        for (u32 layer = 0u; layer < 4u; ++layer) {
                            stage.m_background_layers[layer] = GridBackgroundLayer{
                                .m_texture_index = layer,
                                .m_order = layer,
                                .m_color_rgba = 0xffffffffu,
                                .m_repeat_period_m = periods[layer],
                                .m_parallax_factor = parallax[layer]};
                        }
                    }

                    // Terrain material 2 occupies one interior pool cell and the
                    // fixture's projection nominates no winner there, so terrain
                    // owns the cell and excludes the particle density field.
                    GridTerrainVisual terrain{};
                    terrain.m_material_id = 2u;
                    terrain.m_color_rgba = 0xff0000ffu;
                    terrain.m_ior = 1.0f;
                    stage.m_terrain_visuals.push_back(terrain);
                    stage.m_terrain_visuals.push_back(water);
                    stage.m_tiles.push_back(
                        GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
                    GridPass::GridStagedUpload upload{};
                    upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
                    upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
                    stage.m_uploads.push_back(std::move(upload));
                    auto rows = std::make_shared<Array<GridParticleInstance>>();
                    for (u32 row = 0u; row < frozen[state].size(); ++row) {
                        const auto &particle = frozen[state][row];
                        // Match makeMatterState/captureMovingMatter: origin 3 cells,
                        // h=1/3m, thickness=1m, no axis inversion or position edits.
                        rows->push_back(
                            {u64(row) + 1u, (double(particle.m_position_x) - 3.0) / 3.0,
                                (double(particle.m_position_y) - 3.0) / 3.0, double(particle.m_volume) / 9.0,
                                double(particle.m_mass) * 1000.0 / 9.0, 300.0, 1u});
                    }

                    stage.m_particle_batches = {GridParticleBatch{0u, std::move(rows)}};
                    PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
                    PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
                    for (;;) {
                        const auto prepared = grid.tryPrepareCellUploads(device, renderer.completedFenceValue());
                        PPR_TEST_ASSERT(prepared.has_value());
                        if (*prepared) {
                            break;
                        }

                        const auto submitted = renderer.lastSubmittedFenceValue();
                        if (submitted != kFenceValueNone) {
                            PPR_TEST_ASSERT(not renderer.waitForFenceValue(submitted));
                        }

                        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
                    }
                    PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
                    const auto draw = [&](const DrawContext &context) -> std::error_code {
                        PPR_RETURN_ERROR_ON_FAIL(MatterPoolRender, grid.renderBackground(context));
                        PPR_RETURN_ERROR_ON_FAIL(MatterPoolRender, grid.render(context));
                        return grid.renderParticles(context);
                    };

                    const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                        PPR_RETURN_ERROR_ON_FAIL(
                            MatterPoolRender, grid.prepareBackgroundInputs(context, snapshot, viewport, scissor));
                        PPR_RETURN_ERROR_ON_FAIL(
                            MatterPoolRender, grid.prepareBackground(context, snapshot, viewport, scissor));
                        return grid.prepareGpuParticles(context);
                    };

                    DrawSubmission submission{"frozen actual solver pool", draw};
                    submission.m_viewport = viewport;
                    submission.m_scissor = scissor;
                    PPR_TEST_ASSERT(
                        not renderer.renderToTexture(*target.get(), {submission}, {}, PreRenderCallback{prepare}));
                    const auto submitted = renderer.lastSubmittedFenceValue();
                    grid.notifyFrameSubmitted(submitted);
                    PPR_TEST_ASSERT(not renderer.waitForFenceValue(submitted));
                    PPR_TEST_ASSERT(not renderer.retireCompletedFrames());

                    shader::ComPtr<ISlangBlob> pixels;
                    rhi::SubresourceLayout layout{};
                    PPR_TEST_ASSERT(
                        not make_error_code(device.readTexture(target.get(), 0u, 0u, pixels.writeRef(), &layout)));
                    const auto *const bytes = static_cast<const u8 *>(pixels->getBufferPointer());
                    const u32 index = state;
                    std::ofstream ppm;
                    if (qa != nullptr and *qa != '\0') {
                        ppm.open(
                            std::filesystem::path{qa} /
                                std::format(
                                    "solver-pool-epoch-{}-native{}.ppm", epochs[state], glossy ? "-glossy" : ""),
                            std::ios::binary);
                        ppm << "P6\n512 512\n255\n";
                    }

                    const auto native_cell = [](const double source) {
                        return static_cast<u32>(std::floor(source - 3.0));
                    };
                    for (u32 y = 0u; y < 512u; ++y) {
                        for (u32 x = 0u; x < 512u; ++x) {
                            const auto *const pixel = bytes + y * layout.rowPitch + x * 4u;
                            const double source_x = (2.0 + (double(x) + 0.5) * 12.0 / 512.0) * 3.0 + 3.0;
                            const double source_y = (14.0 - (double(y) + 0.5) * 12.0 / 512.0) * 3.0 + 3.0;
                            // Inventory is 360 source-cell areas in a 24-cell-wide
                            // basin (15 cells deep). This deep ROI excludes walls by
                            // >=5 source cells, floor by >=4, and free surface by >=7.
                            // A cell that nominated a particle must be fully painted.
                            // A cell that nominated none is judged against the
                            // reconstruction's influence radius instead: its paint is
                            // counted by Chebyshev distance band and bounded by the
                            // kernel, not assumed to be absent.
                            if (not glossy and
                                source_x > 17.0 and
                                source_x < 31.0 and
                                source_y > 16.0 and
                                source_y < 20.0) {
                                ++samples[index];
                                const u32 cell_x = native_cell(source_x);
                                const u32 cell_y = native_cell(source_y);
                                const u32 cell = cell_y * kPoolEdge + cell_x;
                                const bool blue = pixel[2] >= 200u;
                                // Two separable label-masked Gaussian passes smooth
                                // the accumulated label and attribute field, so a
                                // particle's paint deliberately extends past its own
                                // cell. The strict interior judgement therefore
                                // applies outside a one-pixel boundary band on both
                                // axes; interior holes stay strict over every sampled
                                // pixel and band paint is reported separately.
                                const double fraction_x = (source_x - 3.0) - double(cell_x);
                                const double fraction_y = (source_y - 3.0) - double(cell_y);
                                const bool cell_interior = fraction_x > pool_edge_band and
                                                           fraction_x < 1.0 - pool_edge_band and
                                                           fraction_y > pool_edge_band and
                                                           fraction_y < 1.0 - pool_edge_band;
                                if (cell_x == kPrecedenceX and cell_y == kPrecedenceY) {
                                    if (cell_interior) {
                                        ++precedence_samples[index];
                                        precedence_ok[index] += pixel[0] > pixel[2] ? 1u : 0u;
                                    } else {
                                        ++precedence_edge_band_samples[index];
                                    }
                                } else if (best_row[cell] != 0xffffffffu) {
                                    ++water_pixels[index];
                                    holes[index] += blue ? 0u : 1u;
                                } else if (cell_interior) {
                                    const u32 band = std::clamp(nomination_distance[cell], 1u, kPoolBands);
                                    outside_band_painted[index][band - 1u] += blue ? 1u : 0u;
                                } else {
                                    outside_edge_band_painted[index] += blue ? 1u : 0u;
                                }
                            }

                            if (not glossy and
                                source_x > 17.0 and
                                source_x < 31.0 and
                                source_y > 40.0 and
                                source_y < 43.0) {
                                far_field_painted[index] += pixel[2] >= 200u ? 1u : 0u;
                            }

                            if (ppm.is_open()) {
                                ppm.write(reinterpret_cast<const char *>(pixel), 3);
                            }
                        }
                    }

                    if (ppm.is_open()) {
                        PPR_TEST_ASSERT(ppm.good());
                    }

                    if (glossy) {
                        // Authored appearance is visual QA, never a false-color
                        // coverage classification or an automatic aesthetic claim.
                        _.log(
                            std::format(
                                "SOLVER-POOL-GLOSSY epoch={} hash={} authored_water=true", epochs[state], hash));
                        continue;
                    }

                    _.log(
                        std::format(
                            "SOLVER-POOL spacing_source=1 cold_volume=1 physical_spacing_m=1/3 physical_volume_m3=1/9 epoch={} tick={} rows={} hash={} rms_displacement_cells={} roi_source=[17,31]x[16,20] samples={} holes={} outside_band1_painted={} outside_band2_painted={} outside_band3_painted={} outside_band4plus_painted={} outside_edge_band_painted={} far_field_painted={} precedence={}/{} precedence_edge_band={}",
                            epochs[state], state == 0u ? 1u : 120u, frozen[state].size(), hash,
                            std::sqrt(rms_motion_squared / double(frozen[state].size())), samples[index], holes[index],
                            outside_band_painted[index][0u], outside_band_painted[index][1u],
                            outside_band_painted[index][2u], outside_band_painted[index][3u],
                            outside_edge_band_painted[index], far_field_painted[index], precedence_ok[index],
                            precedence_samples[index], precedence_edge_band_samples[index]));
                }
            }
        }

        // Emit all early/late native evidence even against old shaders.
        //
        // The accepted bound is derived from the reconstruction kernel, not from
        // an observed count. One particle fills one native cell here, so its
        // reference radius is r = sqrt(V/pi) = sqrt(1/pi) = 0.5642 cell and the
        // accumulated support is R_surface = kParticleSurfaceRadiusScale * r =
        // 1.5259485586443693 * 0.5642 = 0.8609 cell. The two separable
        // label-masked Gaussian passes then spread that accumulation by
        // sigma = 0.15 cell truncated at +/-3 sigma, i.e. 3 * kParticleSurfaceSmoothingSigma
        // = 3 * 0.15 = 0.45 cell. The furthest a painted pixel can therefore lie
        // from the centre of a particle that nominated a cell is
        // 0.8609 + 0.45 = 1.3109 cell, and a pixel that close sits at most
        // ceil(1.3109) = 2 cells away from that nominated cell. Bands 1-2 may
        // legitimately receive that bleed and are reported; band 3 and beyond
        // cannot, so they must stay unpainted. The far-field probe lies outside
        // every support and must stay unpainted in all bands.
        for (u32 index = 0u; index < holes.size(); ++index) {
            PPR_TEST_ASSERT(samples[index] > 0u);
            PPR_TEST_ASSERT(outside_band_painted[index][2u] == 0u); // band 3
            PPR_TEST_ASSERT(outside_band_painted[index][3u] == 0u); // band 4+
            PPR_TEST_ASSERT(far_field_painted[index] == 0u); // beyond all support
            PPR_TEST_ASSERT(holes[index] == 0u);
            PPR_TEST_ASSERT(precedence_samples[index] > 0u);
            PPR_TEST_ASSERT(precedence_ok[index] == precedence_samples[index]);
            PPR_TEST_ASSERT(water_pixels[index] > 0u);
        }
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

        for (const float field: std::bit_cast<std::array<float, 24u>>(captured_value))
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

    // ------------------------------------------------------------------
    // transport-exit recovery through the production helper
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (transport_exit_repair_is_bounded_and_a_no_op_when_certified) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        // Case 3 is the straight bottom-half face y = 0.5 across x in [0, 1], with
        // a single canonical segment. The production helper projects an uncertified
        // published point onto that face.
        const std::array<u32, 1u> masks{3u};
        const TerrainView face{masks, 1u, 1u, 0.0f, 0.0f};
        const auto bits = [](const float value) { return std::bit_cast<u32>(value); };
        const auto probe = [&](const float x, const float y) {
            const auto result = repairProbe(fixture, face, {x, y});
            PPR_TEST_ASSERT(result.m_errors[0u] == 0u and result.m_contacts[11u] == 0u);
            return result;
        };

        // One representable step inside the face: the input is uncertified, the
        // published point is the certified canonical feature point (zero outward
        // steps, therefore inside the three-step bound).
        const float one_ulp_inside = std::nextafter(0.5f, 0.0f);
        PPR_TEST_ASSERT(std::nextafter(one_ulp_inside, std::numeric_limits<float>::infinity()) == 0.5f);
        const auto near_face = probe(0.5f, one_ulp_inside);
        PPR_TEST_ASSERT(near_face.m_contacts[3u] == 0u);
        PPR_TEST_ASSERT(near_face.m_contacts[2u] == 1u);
        PPR_TEST_ASSERT(near_face.m_contacts[4u] == 0u);
        PPR_TEST_ASSERT(near_face.m_contacts[0u] == bits(0.5f) and near_face.m_contacts[1u] == bits(0.5f));
        PPR_TEST_ASSERT(near_face.m_contacts[5u] == bits(0.5f) and near_face.m_contacts[6u] == bits(0.5f));
        PPR_TEST_ASSERT(near_face.m_contacts[10u] == 1u);

        // Deep penetration (a quarter cell) snaps to the same nearest certified
        // face: bounded by that face distance, never a silent clamp to the input.
        const auto deep = probe(0.5f, 0.25f);
        PPR_TEST_ASSERT(deep.m_contacts[3u] == 0u and deep.m_contacts[2u] == 1u);
        PPR_TEST_ASSERT(deep.m_contacts[4u] == 0u);
        PPR_TEST_ASSERT(deep.m_contacts[8u] == bits(0.5f) and deep.m_contacts[9u] == bits(0.25f));
        PPR_TEST_ASSERT(deep.m_contacts[0u] == deep.m_contacts[5u] and deep.m_contacts[1u] == deep.m_contacts[6u]);
        PPR_TEST_ASSERT(
            std::abs(std::bit_cast<float>(deep.m_contacts[1u]) - std::bit_cast<float>(deep.m_contacts[9u])) <= 0.5f);

        // A certified position is returned bit-identically: the repair is a no-op
        // on a passing tick, so a successful transport exit is unchanged by B.
        const auto free = probe(0.5f, 0.75f);
        PPR_TEST_ASSERT(free.m_contacts[3u] == 1u and free.m_contacts[2u] == 1u);
        PPR_TEST_ASSERT(free.m_contacts[4u] == 1u);
        PPR_TEST_ASSERT(free.m_contacts[0u] == free.m_contacts[8u] and free.m_contacts[1u] == free.m_contacts[9u]);
        PPR_TEST_ASSERT(free.m_contacts[0u] == bits(0.5f) and free.m_contacts[1u] == bits(0.75f));
    };

    // ------------------------------------------------------------------
    // recoverable terrain-position verdict
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_position_rejection_is_recoverable_and_other_verdicts_stay_fatal) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        GpuParticle particle = restParticle();
        particle.m_mass = 1.0e-6f;
        fixture.replace({&particle, 1u});
        const u64 epoch = fixture.m_solver.accounting().m_epoch;
        const auto committed = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(committed and committed->m_epoch == epoch and committed->m_count == 1u);

        // One submitted working batch, one injected verdict, one poll.
        const auto poll_verdict = [&](const std::array<u32, 6u> &words) {
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            fixture.m_solver.injectTickVerdictForTest(words);
            auto outcome = fixture.m_solver.pollTick();
            fixture.m_solver.injectTickVerdictForTest(std::nullopt);
            return outcome;
        };

        // flags 256 / claimed / field 6 is the single recoverable verdict.
        const auto recoverable = poll_verdict({256u, 1u, 0xffff'ffffu, 6u, 4455u, 0xffff'ffffu});
        PPR_TEST_ASSERT(recoverable.has_value());
        PPR_TEST_ASSERT(recoverable->rejectedTerrainPosition() and not *recoverable and not recoverable->pending());
        PPR_TEST_ASSERT(
            fixture.m_solver.failure().m_flags == 256u and
            fixture.m_solver.failure().m_claimed == 1u and
            fixture.m_solver.failure().m_field == 6u and
            fixture.m_solver.failure().m_particle == 4455u);

        // A verdict is not a failure: no cause, no epoch bump, no publication.
        PPR_TEST_ASSERT(fixture.m_solver.failure().m_cause == std::error_code{});
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
        PPR_TEST_ASSERT(committed->m_live_epoch->load() == epoch);
        const auto unchanged = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(unchanged and unchanged->m_hot == committed->m_hot and unchanged->m_cold == committed->m_cold);

        // The rejected batch is discarded through the existing path, and the same
        // committed epoch then still submits and publishes.
        PPR_TEST_ASSERT(not fixture.m_solver.discardTick());
        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto resumed = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(resumed and *resumed);
        fixture.m_solver.publishTick();
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);

        // Every other verdict stays fatal: flag 8, flag 264 (256|8), the other
        // 256 fields including the deliberately excluded 3 and 7, and an
        // unclaimed 256 word with no decoded field.
        const u64 rejected_epoch = fixture.m_solver.accounting().m_epoch;
        const auto fatal = [&](const std::array<u32, 6u> &words) {
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(rejected_epoch, 1u, 2u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());
            fixture.m_solver.injectTickVerdictForTest(words);
            const auto outcome = fixture.m_solver.pollTick();
            fixture.m_solver.injectTickVerdictForTest(std::nullopt);
            PPR_TEST_ASSERT(not outcome and outcome.error() == std::errc::result_out_of_range);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == rejected_epoch);
            PPR_TEST_ASSERT(not fixture.m_solver.discardTick());
        };

        fatal({8u, 1u, 0xffff'ffffu, 6u, 4455u, 0xffff'ffffu});
        fatal({264u, 1u, 0xffff'ffffu, 6u, 4455u, 0xffff'ffffu});
        for (const u32 field: {1u, 2u, 3u, 4u, 5u, 7u, 8u})
            fatal({256u, 1u, 0xffff'ffffu, field, 4455u, 0xffff'ffffu});
        fatal({256u, 0u, 0xffff'ffffu, 6u, 4455u, 0xffff'ffffu});
        PPR_TEST_ASSERT(fixture.m_solver.failure().m_cause == std::errc::result_out_of_range);
    };

    PPR_UNIT_TEST (resident_append_copies_the_current_prefix_and_uploads_only_new_rows) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };

        const auto validation_setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
        const bool validation = not validation_setting or *validation_setting == "1";
        fixture.initialize(false, 32u, 32u, validation);

        // Published zero-row storage is a warm donor, not another bootstrap.
        // Replacement must still allocate distinct mutable row/sparse storage.
        const auto initial_empty = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(initial_empty and initial_empty->m_count == 0u);

        fixture.replace({});
        const auto replaced_empty = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(replaced_empty and replaced_empty->m_count == 0u);
        PPR_TEST_ASSERT(replaced_empty->m_hot != initial_empty->m_hot);
        PPR_TEST_ASSERT(replaced_empty->m_cold != initial_empty->m_cold);

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

        const auto old_reader = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(old_reader and old_reader->m_epoch == epoch);

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

        const auto after_cancel = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(after_cancel and after_cancel->m_hot == old_reader->m_hot);
        PPR_TEST_ASSERT(after_cancel->m_cold == old_reader->m_cold);
        PPR_TEST_ASSERT(old_reader->m_live_epoch->load() == old_reader->m_epoch);

        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend({&appended, 1u}, epoch, {}));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto prepared = fixture.m_solver.pollReplacement();
        PPR_TEST_ASSERT(prepared and *prepared);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);

        fixture.m_solver.publishReplacement();
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_particle_count == 2u);

        const auto appended_reader = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(appended_reader and appended_reader->m_hot != old_reader->m_hot);
        PPR_TEST_ASSERT(appended_reader->m_cold != old_reader->m_cold);
        PPR_TEST_ASSERT(old_reader->m_live_epoch->load() != old_reader->m_epoch);

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

        // Full nonempty replacement uses the same immutable pairs after its
        // donor owner is retired, while retaining independent mutable buffers.
        const auto replacement_rows = fixture.snapshot();
        const auto previous_generation = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(previous_generation.has_value());

        fixture.replace(replacement_rows);
        const auto replacement_view = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(replacement_view and replacement_view->m_hot != previous_generation->m_hot);
        PPR_TEST_ASSERT(replacement_view->m_cold != previous_generation->m_cold);

        const auto replacement_snapshot = fixture.snapshot();
        PPR_TEST_ASSERT(replacement_snapshot.size() == replacement_rows.size());
        PPR_TEST_ASSERT(
            std::memcmp(
                replacement_snapshot.data(), replacement_rows.data(), replacement_rows.size() * sizeof(GpuParticle)) ==
            0);

        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch + 3u, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto replacement_tick = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(replacement_tick and *replacement_tick);

        fixture.m_solver.publishTick();
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 4u);
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

        for (u32 field = 0u; field < 24u; ++field) {
            auto scalars = std::bit_cast<std::array<float, 24u>>(valid);
            scalars[field] = std::numeric_limits<float>::quiet_NaN();
            rows.push_back(std::bit_cast<GpuParticle>(scalars));
            expected.push_back(field);
        }

        const auto add_invalid = [&](const u32 field, const float value) {
            auto scalars = std::bit_cast<std::array<float, 24u>>(valid);
            scalars[field] = value;
            rows.push_back(std::bit_cast<GpuParticle>(scalars));
            expected.push_back(field);
        };

        add_invalid(0u, 2.999f);
        add_invalid(1u, 28.001f);
        add_invalid(12u, 0.0f);
        add_invalid(13u, -1.0f);
        add_invalid(14u, 6.0f);
        add_invalid(15u, 0.0f);
        add_invalid(21u, 0.0f);
        add_invalid(22u, -1.0f);
        add_invalid(23u, 1.0f);

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
        PPR_TEST_ASSERT(hot->getFieldByIndex(10u)->getOffset() == PPR_OFFSETOF(ParticleWrite, m_compaction));
        PPR_TEST_ASSERT(hot->getFieldByIndex(11u)->getOffset() == PPR_OFFSETOF(ParticleWrite, m_solid_fraction));
        PPR_TEST_ASSERT(cold->getFieldByIndex(0u)->getOffset() == PPR_OFFSETOF(ParticleReadonly, m_mass));
        PPR_TEST_ASSERT(cold->getFieldByIndex(3u)->getOffset() == PPR_OFFSETOF(ParticleReadonly, m_color_r));
        PPR_TEST_ASSERT(cold->getFieldByIndex(6u)->getOffset() == PPR_OFFSETOF(ParticleReadonly, m_property_row));

        shader::SharedModule maintenance;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "sparse_grid.slang", "pbmpm_sparse_grid",
            maintenance.writeRef()));
        auto *const maintenance_layout = maintenance->getLayout();
        PPR_TEST_ASSERT(maintenance_layout != nullptr);
        slang::TypeLayoutReflection *maintenance_cold = nullptr;
        for (unsigned index = 0u; index < maintenance_layout->getParameterCount(); ++index) {
            auto *const parameter = maintenance_layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_cold")
                maintenance_cold = parameter->getTypeLayout()->getElementTypeLayout();
        }

        PPR_TEST_ASSERT(maintenance_cold != nullptr);
        PPR_TEST_ASSERT(maintenance_cold->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ParticleReadonly));
        PPR_TEST_ASSERT(
            maintenance_cold->getFieldByIndex(6u)->getOffset() == PPR_OFFSETOF(ParticleReadonly, m_property_row));

        auto *const properties = find("g_properties");
        PPR_TEST_ASSERT(properties != nullptr);
        PPR_TEST_ASSERT(properties->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GpuMaterialProperties));
        PPR_TEST_ASSERT(
            properties->getFieldByIndex(8u)->getOffset() == PPR_OFFSETOF(GpuMaterialProperties, m_reference_density));
        PPR_TEST_ASSERT(
            properties->getFieldByIndex(9u)->getOffset() == PPR_OFFSETOF(GpuMaterialProperties, m_sand_yield_ratio));
        PPR_TEST_ASSERT(
            properties->getFieldByIndex(10u)->getOffset() == PPR_OFFSETOF(GpuMaterialProperties, m_cohesion));
        PPR_TEST_ASSERT(
            properties->getFieldByIndex(18u)->getOffset() == PPR_OFFSETOF(GpuMaterialProperties, m_dense_epsilon));

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
                                         sizeof(GpuMaterialProperties) +
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

    // ------------------------------------------------------------------
    // immutable substance table behavior
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (calibrated_caps_hardening_and_two_regime_edges) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);

        constexpr u32 count = 22u;
        std::array<GpuParticle, count> input{};
        std::array<GpuMaterialProperties, count> properties{};
        const auto set = [&](const u32 index, const ESourceMaterial family, const float t, const float n, const float C,
                             const float phi) {
            auto &p = input[index];
            p = restParticle();
            p.m_material = static_cast<float>(family);
            p.m_log_jp = 0.0f;
            p.m_compaction = C;
            p.m_solid_fraction = phi;
            p.m_deformation_gradient = {
                std::exp(t * 0.5f + n / std::sqrt(2.0f)), 0.0f, 0.0f, std::exp(t * 0.5f - n / std::sqrt(2.0f))};

            properties[index].m_substance_id = index + 1u;
            properties[index].m_family = family;
        };

        const float snow_cap = std::log(0.99f / 0.3f);
        const float compacted_phi = 0.3f + (0.99f - 0.3f) * (1.0f - std::exp(-0.5f * (2.0f - snow_cap)));
        set(0u, ESourceMaterial::cohesive, -0.8f, 0.0f, 0.0f, 0.3f);
        set(1u, ESourceMaterial::cohesive, -0.8f, 0.0f, 0.2f, 0.3f);
        set(2u, ESourceMaterial::cohesive, 0.0f, 0.0f, 0.2f, 0.3f);
        set(3u, ESourceMaterial::cohesive, 0.0f, 0.5f, 0.0f, 0.3f);
        set(4u, ESourceMaterial::cohesive, 0.5f, 0.0f, 0.0f, 0.3f);
        set(5u, ESourceMaterial::compacting, -2.0f, 0.0f, 0.0f, 0.3f);
        set(6u, ESourceMaterial::compacting, 0.0f, 0.0f, 0.0f, compacted_phi);
        set(7u, ESourceMaterial::compacting, 0.0f, 0.5f, 0.0f, 0.3f);
        set(8u, ESourceMaterial::compacting, 0.0f, 0.5f, 0.0f, compacted_phi);
        set(9u, ESourceMaterial::compacting, 0.0f, 0.5f, 0.0f, std::nextafter(0.99f, 0.0f));
        set(10u, ESourceMaterial::compacting, 0.0f, 0.5f, 0.0f, 0.99f);
        set(11u, ESourceMaterial::compacting, -30.0f, 0.5f, 0.0f, 0.99f);
        set(12u, ESourceMaterial::cohesive, 0.0f, 0.0f, 0.0f, 0.3f);
        input[12u].m_deformation_gradient.m_c0x = 0.0f;
        set(13u, ESourceMaterial::compacting, -0.1f, 0.5f, 0.0f, 0.3f);
        // Start porous and cross the exact dense boundary in this return while
        // shear is active. The dense limit clamps t to zero and sets n=c.
        set(14u, ESourceMaterial::compacting, -2.0f, 1.0f, 0.0f, 0.3f);
        properties[14u].m_compaction_rate = 100.0f;

        // Compression is bounded by porosity on both history paths. An excessive
        // tensile return still outside the exponent band is a reported defect.
        set(15u, ESourceMaterial::compacting, -160.0f, 0.0f, 0.0f, std::nextafter(0.99f, 0.0f));
        set(16u, ESourceMaterial::compacting, -160.0f, 0.0f, 0.0f, 0.99f);
        set(17u, ESourceMaterial::cohesive, -160.0f, 0.0f, 1000.0f, 0.3f);
        set(18u, ESourceMaterial::cohesive, 160.0f, 0.0f, 0.0f, 0.3f);
        properties[18u].m_tension = 200.0f;

        // A vanishing shear edge is a valid projection, including n=0. Only
        // the separate nonfinite-input control must reject before logarithms.
        set(19u, ESourceMaterial::compacting, 0.0f, 0.0f, 0.0f, 0.99f);
        properties[19u].m_cohesion = 0.0f;
        set(20u, ESourceMaterial::compacting, 0.0f, 0.5f, 0.0f, 0.99f);
        properties[20u].m_cohesion = 0.0f;
        set(21u, ESourceMaterial::compacting, 0.0f, 0.0f, 0.0f, 0.99f);
        input[21u].m_deformation_gradient.m_c0x = std::numeric_limits<float>::infinity();

        shader::SharedModule module;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "property_checks.slang", "pbmpm_law_checks",
            module.writeRef()));
        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_TEST_ASSERT(
            not make_error_code(module->findEntryPointByName("inspectCalibratedReturns", entry.writeRef())));
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

        const auto buffer = [&](const void *data, const u64 size, const u32 stride, const bool writable) {
            rhi::BufferDesc desc{};
            desc.size = size;
            desc.elementSize = stride;
            desc.usage = writable ? static_cast<rhi::BufferUsage>(
                                        static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                                        static_cast<u32>(rhi::BufferUsage::CopySource))
                                  : rhi::BufferUsage::ShaderResource;
            desc.defaultState = writable ? rhi::ResourceState::UnorderedAccess : rhi::ResourceState::ShaderResource;
            rhi::ComPtr<rhi::IBuffer> result;
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, data, result.writeRef())));
            return result;
        };

        const auto source = buffer(input.data(), sizeof(input), sizeof(GpuParticle), false);
        const auto table = buffer(properties.data(), sizeof(properties), sizeof(GpuMaterialProperties), false);
        const auto output = buffer(nullptr, sizeof(input), sizeof(GpuParticle), true);
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        auto *const root = pass->bindPipeline(pipeline);
        PPR_TEST_ASSERT(root != nullptr);
        rhi::ShaderCursor cursor{root};
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_law_input"].setBinding(rhi::Binding(source, {.offset = 0u, .size = sizeof(input)}))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_properties"].setBinding(rhi::Binding(table, {.offset = 0u, .size = sizeof(properties)}))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_law_output"].setBinding(rhi::Binding(output, {.offset = 0u, .size = sizeof(input)}))));
        pass->dispatchCompute(1u, 1u, 1u);
        pass->end();

        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
        PPR_DEFER {
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        };
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        std::array<GpuParticle, count> result{};
        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(output, 0u, sizeof(result), result.data())));

        const auto t = [&](const u32 i) {
            return std::log(result[i].m_deformation_gradient.m_c0x) + std::log(result[i].m_deformation_gradient.m_c1y);
        };
        const auto n = [&](const u32 i) {
            return std::abs(
                       std::log(result[i].m_deformation_gradient.m_c0x) -
                       std::log(result[i].m_deformation_gradient.m_c1y)) /
                   std::sqrt(2.0f);
        };

        for (u32 i = 0u; i < 12u; ++i) {
            if (result[i].m_enabled != 1.0f) {
                std::printf(
                    "CALIBRATED-RETURN first_disabled=%u frozen_rejection=%.9g\n", i,
                    static_cast<double>(result[i].m_liquid_density));
                const auto input_words = std::bit_cast<std::array<float, 24u>>(input[i]);
                const auto output_words = std::bit_cast<std::array<float, 24u>>(result[i]);
                for (std::size_t word = 0u; word < output_words.size(); ++word)
                    std::printf(
                        "  scalar[%zu] input=%.9g output=%.9g\n", word, static_cast<double>(input_words[word]),
                        static_cast<double>(output_words[word]));

                std::fflush(stdout);
            }

            PPR_TEST_ASSERT(result[i].m_enabled == 1.0f);
        }

        // Analytical diagonal returns: sigma = exp(t/2 +/- n/sqrt(2)), F = diag(sigma).
        // Cohesive D=min(0.2+0.5*C, log(0.99/phi)); C'=max(C,-min(t',0)).
        // Porous D=log(0.99/phi), recomputed after the bounded phi update.
        const float dense_edge = 0.05f * 0.99f * 0.99f;
        const std::array<std::array<float, 2u>, 12u> expected_trace_norm{{
            {-0.2f, 0.0f}, // 0: cohesive axial cap; C'=0.2, c'=0.09, no shear.
            {-0.3f, 0.0f}, // 1: hardened axial cap; C'=0.3, c'=0.11, no shear.
            {0.0f, 0.0f}, // 2: unloaded hardened clay; identity, C'=0.2.
            {0.0f, 0.05f}, // 3: cohesive shear-only edge c=0.05.
            {0.02f, 0.0f}, // 4: tension clamp from t=0.5 to T=0.02.
            {-std::log(0.99f / compacted_phi), 0.0f}, // 5: updated porous axial cap; ellipse edge zero.
            {0.0f, 0.0f}, // 6: unloaded compacted snow; identity, phi unchanged.
            {0.0f, 0.05f * 0.3f * 0.3f}, // 7: porous ellipse at t=0; n'=c*phi^2.
            {0.0f, 0.05f * compacted_phi * compacted_phi}, // 8: same edge after compaction.
            {0.0f, dense_edge}, // 9: one ulp below phiDense is inside the dense rounding band.
            {0.0f, dense_edge}, // 10: exact dense cone; n'=c*phiDense^2.
            {0.0f, dense_edge}, // 11: dense cone removes t=-30 compression, retaining limited shear.
        }};

        // These well-conditioned diagonal projections have |projected| <= 0.354.
        // 1e-6 relative allows about eight FP32 epsilons for CPU/GPU log/exp,
        // history exp and reconstruction; no near-cap square-root sensitivity
        // enters these nonzero shear targets (case 5 has n=0).
        for (u32 i = 0u; i < expected_trace_norm.size(); ++i) {
            const auto &expected = expected_trace_norm[i];
            const float sigma_x = std::exp(expected[0u] * 0.5f + expected[1u] / std::sqrt(2.0f));
            const float sigma_y = std::exp(expected[0u] * 0.5f - expected[1u] / std::sqrt(2.0f));
            const auto &F = result[i].m_deformation_gradient;
            PPR_TEST_ASSERT(std::abs(F.m_c0x - sigma_x) <= sigma_x * 1e-6f);
            PPR_TEST_ASSERT(std::abs(F.m_c1y - sigma_y) <= sigma_y * 1e-6f);
            PPR_TEST_ASSERT(F.m_c0y == 0.0f and F.m_c1x == 0.0f);
        }

        // Zero singular value rejects before log; neither history nor sigma commits.
        PPR_TEST_ASSERT(result[12u].m_enabled == 0.0f and result[12u].m_liquid_density == 27.0f);
        PPR_TEST_ASSERT(result[12u].m_deformation_gradient.m_c0x == 0.0f);
        PPR_TEST_ASSERT(result[12u].m_deformation_gradient.m_c1y == 1.0f);
        PPR_TEST_ASSERT(result[12u].m_compaction == 0.0f and result[12u].m_solid_fraction == 0.3f);
        const float sine = std::sin(30.0f / 180.0f * 3.14159f);
        const float alpha = std::sqrt(2.0f / 3.0f) * 2.0f * sine / (3.0f - sine);
        const float elliptical_edge = (alpha * 0.1f + 0.05f * 0.3f * 0.3f) *
                                      std::sqrt(1.0f - (0.1f / snow_cap) * (0.1f / snow_cap));
        PPR_TEST_ASSERT(result[13u].m_enabled == 1.0f and std::abs(n(13u) - elliptical_edge) < 1e-5f);

        PPR_TEST_ASSERT(std::abs(t(0u) + 0.2f) < 1e-5f and std::abs(result[0u].m_compaction - 0.2f) < 1e-5f);
        PPR_TEST_ASSERT(std::abs(t(1u) + 0.3f) < 1e-5f and result[1u].m_compaction > result[0u].m_compaction);
        PPR_TEST_ASSERT(result[2u].m_compaction == input[2u].m_compaction);
        PPR_TEST_ASSERT(std::abs(t(3u)) < 1e-5f and std::abs(n(3u) - 0.05f) < 1e-5f);
        PPR_TEST_ASSERT(std::abs(t(4u) - 0.02f) < 1e-5f);
        PPR_TEST_ASSERT(
            std::abs(t(5u) + std::log(0.99f / compacted_phi)) < 1e-5f and
            std::abs(result[5u].m_solid_fraction - compacted_phi) < 1e-5f);
        PPR_TEST_ASSERT(result[6u].m_solid_fraction == compacted_phi and n(8u) > n(7u));
        // Both near-dense and exact-dense inputs select the cone, whose
        // zero-confinement yield edge is n=c*phiDense^2.
        // The finite-compression ellipse edge is intentionally NOT continuous.
        PPR_TEST_ASSERT(std::abs(n(9u) - n(10u)) < 1e-5f);
        PPR_TEST_ASSERT(std::abs(t(11u)) < 1e-5f and result[11u].m_solid_fraction == 0.99f);

        for (u32 i = 15u; i < 18u; ++i) {
            const float expected = i == 17u ? std::sqrt(0.3f / 0.99f) : 1.0f;
            PPR_TEST_ASSERT(result[i].m_enabled == 1.0f and result[i].m_liquid_density == 0.0f);
            // The dense cases clamp compression away and reconstruct F through
            // the SVD plus an exp/log round trip; measured residue is ~2.9e-6 at
            // unit magnitude, so 1e-5 relative (a few FP32 ulps per
            // transcendental) is the honest scale for all three identities; the
            // cohesive case measures 2.8e-6 residual for the same reason.
            const float round_trip = 1e-5f;
            PPR_TEST_ASSERT(std::abs(result[i].m_deformation_gradient.m_c0x - expected) <= expected * round_trip);
            PPR_TEST_ASSERT(std::abs(result[i].m_deformation_gradient.m_c1y - expected) <= expected * round_trip);
            PPR_TEST_ASSERT(std::abs(result[i].m_deformation_displacement.m_c0x - expected) <= expected * round_trip);
            PPR_TEST_ASSERT(std::abs(result[i].m_deformation_displacement.m_c1y - expected) <= expected * round_trip);
        }

        PPR_TEST_ASSERT(result[18u].m_enabled == 0.0f and result[18u].m_liquid_density == 24.0f);

        for (u32 i = 19u; i < 21u; ++i) {
            PPR_TEST_ASSERT(result[i].m_enabled == 1.0f and result[i].m_liquid_density == 0.0f);
            PPR_TEST_ASSERT(std::abs(t(i)) < 1e-5f and n(i) < 1e-5f);
        }

        PPR_TEST_ASSERT(result[21u].m_enabled == 0.0f and result[21u].m_liquid_density == 27.0f);
        PPR_TEST_ASSERT(result[17u].m_compaction == input[17u].m_compaction);

        PPR_TEST_ASSERT(result[14u].m_enabled == 1.0f and result[14u].m_solid_fraction == 0.99f);
        PPR_TEST_ASSERT(std::abs(t(14u)) < 1e-5f and std::abs(n(14u) - dense_edge) < 1e-5f);
    };

    PPR_UNIT_TEST (calibrated_admission_and_clay_sand_divergence) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);

        GpuMaterialProperties clay{};
        clay.m_substance_id = 1u;
        clay.m_family = ESourceMaterial::cohesive;
        // Every extension field rejects NaN; signed domains and regime endpoints
        // reject at the public preparation boundary without publishing anything.
        for (u32 field = 10u; field < 19u; ++field) {
            auto words = std::bit_cast<std::array<float, 19u>>(clay);
            for (const float bad: {-1.0f, std::numeric_limits<float>::quiet_NaN()}) {
                words[field] = bad;
                const auto row = std::bit_cast<GpuMaterialProperties>(words);
                PPR_TEST_ASSERT(not validMaterialProperties(row));
            }
        }

        GpuParticle particle = restParticle();
        particle.m_log_jp = 0.0f;
        particle.m_material = 4.0f;
        particle.m_deformation_gradient = {std::exp(0.1f), 0.0f, 0.0f, std::exp(-0.1f)};
        const std::array<u32, 1u> indices{0u};
        for (const float bad: {0.0f, 1.0f}) {
            auto invalid = clay;
            invalid.m_dense_epsilon = bad;
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            PPR_TEST_ASSERT(
                fixture.m_solver.prepareReplacement(
                    {&particle, 1u}, {}, epoch, {}, ETerrainUpdate::replace, {{&invalid, 1u}, indices}) ==
                std::errc::invalid_argument);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
        }

        const auto evolve = [&](const GpuMaterialProperties &property) {
            particle.m_material = static_cast<float>(property.m_family);
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                {&particle, 1u}, {}, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace,
                {{&property, 1u}, indices}));
            fixture.m_solver.publishReplacement();
            fixture.finishOneSubstep();
            return fixture.publishCompletedTick()[0u];
        };

        const auto cohesive = evolve(clay);
        auto sand = clay;
        sand.m_family = ESourceMaterial::sand;
        const auto granular = evolve(sand);
        PPR_TEST_ASSERT(
            std::abs(cohesive.m_deformation_gradient.m_c0x - granular.m_deformation_gradient.m_c0x) > 1e-3f);

        // Restore non-default new-family histories through the public upload
        // boundary, then preserve them through a resident append and re-upload.
        std::array<GpuMaterialProperties, 2u> history_rows{clay, clay};
        history_rows[1u].m_substance_id = 2u;
        history_rows[1u].m_family = ESourceMaterial::compacting;
        std::array<GpuParticle, 2u> history_particles{restParticle(), restParticle()};
        history_particles[0u].m_material = static_cast<float>(ESourceMaterial::cohesive);
        history_particles[0u].m_log_jp = 0.0f;
        history_particles[0u].m_compaction = 0.375f;
        history_particles[0u].m_solid_fraction = 0.625f;
        history_particles[1u].m_material = static_cast<float>(ESourceMaterial::compacting);
        history_particles[1u].m_log_jp = 0.0f;
        history_particles[1u].m_position_x += 8.0f;
        history_particles[1u].m_compaction = 0.125f;
        history_particles[1u].m_solid_fraction = 1.0f - history_rows[1u].m_dense_epsilon;
        const std::array<u32, 2u> history_indices{0u, 1u};
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            std::span{history_particles}.first(1u), {}, fixture.m_solver.accounting().m_epoch, {},
            ETerrainUpdate::replace, {history_rows, std::span{history_indices}.first(1u)}));
        fixture.m_solver.publishReplacement();

        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend(
            std::span{history_particles}.subspan(1u), fixture.m_solver.accounting().m_epoch, {},
            ETerrainUpdate::preserve, {history_rows, std::span{history_indices}.subspan(1u)}));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto copied = fixture.m_solver.pollReplacement();
        PPR_TEST_ASSERT(copied and *copied);
        fixture.m_solver.publishReplacement();

        const auto appended = fixture.snapshot();
        PPR_TEST_ASSERT(appended.size() == history_particles.size());
        PPR_TEST_ASSERT(std::memcmp(appended.data(), history_particles.data(), sizeof(history_particles)) == 0);
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            appended, {}, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace,
            {history_rows, history_indices}));
        fixture.m_solver.publishReplacement();

        const auto restored = fixture.snapshot();
        PPR_TEST_ASSERT(restored.size() == history_particles.size());
        PPR_TEST_ASSERT(std::memcmp(restored.data(), history_particles.data(), sizeof(history_particles)) == 0);
    };

    // ------------------------------------------------------------------
    // loaded calibrated-material geometry diagnostics
    // ------------------------------------------------------------------

    void runCalibratedLoadedScene(const bool diagnostic) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        constexpr float gravity = 9.81f / (64.0f / 3.0f);
        fixture.initialize(true, 64u, 64u, true, nullptr, gravity);

        // A wide floor supports an initially stress-free 4-by-12 column.
        // Source units: h=1/3 m, floor top=12, particle spacing=1.
        const GpuStaticShape floor{
            .m_position_x = 32.0f, .m_position_y = 10.0f, .m_half_size_x = 28.0f, .m_half_size_y = 2.0f};
        const auto patch = [](const ESourceMaterial family) {
            Array<GpuParticle> particles;
            for (u32 y = 0u; y < 12u; ++y) {
                for (u32 x = 0u; x < 4u; ++x) {
                    auto p = restParticle();
                    p.m_position_x = 30.5f + float(x);
                    p.m_position_y = 12.5f + float(y);
                    p.m_material = static_cast<float>(family);
                    p.m_log_jp = 0.0f;
                    particles.push_back(p);
                }
            }

            return particles;
        };
        const auto upload = [&](const Array<GpuParticle> &particles, const GpuMaterialProperties &property) {
            const Array<u32> indices(particles.size(), 0u);
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                particles, {&floor, 1u}, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace,
                {{&property, 1u}, indices}));
            fixture.m_solver.publishReplacement();
        };
        const auto geometry = [](const Array<GpuParticle> &particles) {
            double top = 0.0, bottom = 0.0, min_x = 64.0, max_x = 0.0, phi = 0.0;
            for (std::size_t i = 0u; i < particles.size(); ++i) {
                const auto &p = particles[i];
                for (const float value: std::bit_cast<std::array<float, 24u>>(p))
                    PPR_TEST_ASSERT(std::isfinite(value));

                if (i < 4u)
                    bottom += static_cast<double>(p.m_position_y) / 4.0;

                if (i >= 44u)
                    top += static_cast<double>(p.m_position_y) / 4.0;

                min_x = std::min(min_x, static_cast<double>(p.m_position_x));
                max_x = std::max(max_x, static_cast<double>(p.m_position_x));
                phi += static_cast<double>(p.m_solid_fraction) / static_cast<double>(particles.size());
            }

            // Material-tagged top/bottom layers measure axial shortening even
            // if a few grains become geometric outliers during collapse.
            return std::array<double, 4u>{top - bottom, max_x - min_x, top - 12.0, phi};
        };
        const auto advance = [&](const char *scene, const char *phase, const GpuMaterialProperties &property,
                                 const bool sustained_compression = false) {
            Array<GpuParticle> state;
            const bool snow = property.m_family == ESourceMaterial::compacting;
            const u32 steps = sustained_compression or (diagnostic and snow) ? 600u : (diagnostic ? 480u : 4u);
            const u32 snapshot_interval = diagnostic ? 120u : steps;
            auto previous = snow ? fixture.snapshot() : Array<GpuParticle>{};
            float max_phi = property.m_solid_fraction0;
            for (const auto &particle: previous)
                max_phi = std::max(max_phi, particle.m_solid_fraction);

            for (u32 step = 1u; step <= steps; ++step) {
                PPR_TEST_ASSERT(not fixture.m_solver.busy());
                PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(
                    fixture.m_solver.accounting().m_epoch, 1u, kDefaultIterations));
                PPR_TEST_ASSERT(not fixture.m_solver.drain());

                const auto complete = fixture.m_solver.pollTick();
                if (not complete or not *complete) {
                    const auto &failure = fixture.m_solver.failure();
                    // Use the assertion hook directly: PPR_TEST_ASSERT has no
                    // message argument. This text survives harness excerpts.
                    onTestAssertionFailure(
                        std::format(
                            "complete and *complete: scene={} phase={} step={} flags={} claimed={} node={} field={} "
                            "particle_row={} property_table_rows=1 max_committed_phi={} cause={}",
                            scene, phase, step, failure.m_flags, failure.m_claimed, failure.m_node, failure.m_field,
                            failure.m_particle, max_phi, failure.m_cause.message())
                            .c_str(),
                        std::source_location::current());
                }

                fixture.m_solver.publishTick();
                if (not snow and step % snapshot_interval != 0u)
                    continue;

                state = fixture.snapshot();
                PPR_TEST_ASSERT(state.size() == 48u);
                if (snow) {
                    PPR_TEST_ASSERT(previous.size() == state.size());
                    for (std::size_t row = 0u; row < state.size(); ++row) {
                        const auto &particle = state[row];
                        PPR_TEST_ASSERT(validMaterialState(particle, property));
                        PPR_TEST_ASSERT(particle.m_solid_fraction >= previous[row].m_solid_fraction);
                        PPR_TEST_ASSERT(particle.m_solid_fraction <= 1.0f - property.m_dense_epsilon);
                        PPR_TEST_ASSERT(particle.m_compaction == previous[row].m_compaction);

                        max_phi = std::max(max_phi, particle.m_solid_fraction);
                    }

                    previous = state;
                }

                const auto g = geometry(state);
                if (not diagnostic or step % snapshot_interval != 0u)
                    continue;

                std::printf(
                    "B1 loaded scene=%s phase=%s step=%u layer_height=%.9g width=%.9g top_above_floor=%.9g mean_phi=%.9g\n",
                    scene, phase, step, g[0u], g[1u], g[2u], g[3u]);
            }

            if (snow)
                std::printf(
                    "B1 snow admissibility phase=%s substeps=%u max_phi=%.9g upper_phi=%.9g\n", phase, steps,
                    static_cast<double>(max_phi), static_cast<double>(1.0f - property.m_dense_epsilon));

            return state;
        };

        GpuMaterialProperties property{};
        property.m_substance_id = 1u;
        property.m_family = ESourceMaterial::cohesive;
        upload(patch(property.m_family), property);
        const auto clay = geometry(advance("clay", "gravity_column", property));

        // Same default coefficients, mass, spacing, support and schedule;
        // only the constitutive family changes for the granular control.
        property.m_family = ESourceMaterial::sand;
        upload(patch(property.m_family), property);
        const auto sand = geometry(advance("granular_control", "gravity_column", property));
        if (diagnostic)
            std::printf(
                "B1 loaded clay_vs_sand height_delta=%.9g width_delta=%.9g support_height_delta=%.9g verdict=%s\n",
                clay[0u] - sand[0u], clay[1u] - sand[1u], clay[2u] - sand[2u],
                clay[0u] > sand[0u] and clay[1u] < sand[1u] ? "greater_column_retention" : "not_established");

        property.m_family = ESourceMaterial::compacting;
        const auto fresh = patch(property.m_family);
        upload(fresh, property);
        const auto initial = geometry(fresh);
        auto state = advance("snow", "compression", property, true);
        const auto compressed = geometry(state);

        // Gravity is immutable in the public profile. Restart only the GPU
        // owner at phase boundaries, transferring every captured state word.
        // No prescribed F, velocity, history or position is injected here.
        const auto set_gravity = [&](const float strength) {
            auto profile = fixture.m_solver.profile();
            profile.m_particle_count = 0u;
            profile.m_shape_count = 0u;
            profile.m_gravity_strength = strength;
            PPR_TEST_ASSERT(not fixture.m_solver.shutdown());
            PPR_TEST_ASSERT(not fixture.m_solver.initialize(
                fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
                profile));
            upload(state, property);
            const auto restored = fixture.snapshot();
            PPR_TEST_ASSERT(restored.size() == state.size());
            PPR_TEST_ASSERT(std::memcmp(restored.data(), state.data(), state.size() * sizeof(GpuParticle)) == 0);
        };

        set_gravity(0.0f);
        state = advance("snow", "release", property);
        const auto released = geometry(state);
        const auto released_state = state;
        set_gravity(gravity);
        state = advance("snow", "reload", property);
        const auto reloaded = geometry(state);

        // Replay the identical released particle state and reload profile,
        // changing only the compacting history to its admitted initial value.
        state = released_state;
        for (auto &particle: state) {
            particle.m_solid_fraction = property.m_solid_fraction0;
            particle.m_compaction = 0.0f;
        }

        set_gravity(gravity);
        state = advance("snow", "reload_reset_history", property);
        const auto reset_reloaded = geometry(state);

        // The first load has different geometry/contact/velocity and is context
        // only. Matched reload shortening is a compliance proxy, not a modulus.
        const double first_shortening = (initial[0u] - compressed[0u]) / initial[0u];
        const double reload_shortening = (released[0u] - reloaded[0u]) / initial[0u];
        const double reset_reload_shortening = (released[0u] - reset_reloaded[0u]) / initial[0u];
        const bool retained = compressed[3u] > initial[3u] and released[3u] >= compressed[3u];
        const bool resistant = reset_reload_shortening > 0.0 and reload_shortening < reset_reload_shortening;
        if (diagnostic) {
            std::printf(
                "B1 loaded snow initial_phi=%.9g compressed_phi=%.9g released_phi=%.9g reloaded_phi=%.9g retained_height_loss=%.9g first_shortening=%.9g reload_shortening=%.9g retained_compaction=%s increased_structural_resistance=%s\n",
                initial[3u], compressed[3u], released[3u], reloaded[3u], (initial[0u] - released[0u]) / initial[0u],
                first_shortening, reload_shortening, retained ? "observed" : "not_established",
                retained and resistant ? "observed" : "not_established");

            std::printf(
                "B1 snow matched_reload retained_height=%.9g reset_height=%.9g retained_width=%.9g reset_width=%.9g retained_shortening=%.9g reset_shortening=%.9g retained_support_height=%.9g reset_support_height=%.9g attribution=history_only_all_other_inputs_identical\n",
                reloaded[0u], reset_reloaded[0u], reloaded[1u], reset_reloaded[1u], reload_shortening,
                reset_reload_shortening, reloaded[2u], reset_reloaded[2u]);
        }

        // Uncalibrated material outcomes are reported, never assumed as gates.
    }

    PPR_UNIT_TEST (calibrated_loaded_clay_and_snow_complete_with_finite_state) {
        // Same geometry, rows, K5 and restart path as the report-only diagnostic;
        // 600 compression substeps assert each snow row's admissibility and
        // monotone phi; other phases retain four substeps. Report maximum phi.
        runCalibratedLoadedScene(false);
    };

    PPR_UNIT_TEST (dense_water_magma_snow_resident_ticks_remain_finite) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, 64u, 64u, true, nullptr, 9.81f / (64.0f / 3.0f));

        // Shipped water/magma/snow coefficients, with explicit physical inventory.
        // Interleaved half-cell samples share stencils; a supported snow bed is
        // loaded by descending liquid. Precompacted snow covers both sides of
        // the dense boundary without relying on thousands of settling epochs.
        std::array<GpuMaterialProperties, 3u> properties{};
        for (u32 i = 0u; i < properties.size(); ++i)
            properties[i].m_substance_id = i + 1u;

        properties[0u].m_family = ESourceMaterial::liquid;
        properties[0u].m_liquid_viscosity = 0.01f;
        properties[0u].m_liquid_relaxation = 1.0f;
        properties[1u].m_family = ESourceMaterial::liquid;
        properties[1u].m_liquid_viscosity = 2.0f;
        properties[1u].m_liquid_relaxation = 0.5f;
        properties[1u].m_reference_density = 2.5f;
        properties[2u].m_family = ESourceMaterial::compacting;
        properties[2u].m_reference_density = 0.3f;

        Array<GpuParticle> particles;
        Array<u32> indices;
        for (u32 y = 0u; y < 32u; ++y) {
            for (u32 x = 0u; x < 24u; ++x) {
                const u32 index = y < 8u ? 2u : (x + y) % 3u;
                auto p = restParticle();
                p.m_position_x = 26.25f + 0.5f * float(x);
                p.m_position_y = 12.25f + 0.5f * float(y);
                p.m_material = static_cast<float>(properties[index].m_family);
                p.m_volume = 0.25f;
                p.m_mass = p.m_volume * properties[index].m_reference_density;
                p.m_log_jp = 0.0f;
                p.m_displacement_y = -0.0005f * float(y);
                if (index == 2u)
                    p.m_solid_fraction = x % 2u == 0u ? std::nextafter(0.99f, 0.0f) : 0.99f;

                particles.push_back(p);
                indices.push_back(index);
            }
        }

        const GpuStaticShape floor{
            .m_position_x = 32.0f, .m_position_y = 10.0f, .m_half_size_x = 28.0f, .m_half_size_y = 2.0f};
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            particles, {&floor, 1u}, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace,
            {properties, indices}));
        fixture.m_solver.publishReplacement();

        auto previous = particles;
        for (u32 tick = 0u; tick < 128u; ++tick) {
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, kGameSubsteps, kDefaultIterations));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());

            const auto complete = fixture.m_solver.pollTick();
            if (not complete or not *complete) {
                const auto &failure = fixture.m_solver.failure();
                onTestAssertionFailure(
                    std::format(
                        "mixed water/magma/snow tick={} epoch={} rows={} flags={} field={} particle={} cause={}", tick,
                        epoch, particles.size(), failure.m_flags, failure.m_field, failure.m_particle,
                        failure.m_cause.message())
                        .c_str(),
                    std::source_location::current());
            }

            fixture.m_solver.publishTick();
            auto state = fixture.snapshot();
            PPR_TEST_ASSERT(state.size() == particles.size());
            for (std::size_t row = 0u; row < state.size(); ++row) {
                const auto &p = state[row];
                for (const float value: std::bit_cast<std::array<float, 24u>>(p))
                    PPR_TEST_ASSERT(std::isfinite(value));

                PPR_TEST_ASSERT(validMaterialState(p, properties[indices[row]]));
                PPR_TEST_ASSERT(p.m_mass == particles[row].m_mass and p.m_volume == particles[row].m_volume);
                if (indices[row] == 2u)
                    PPR_TEST_ASSERT(p.m_solid_fraction >= previous[row].m_solid_fraction);
            }

            previous = std::move(state);
        }
    };

    PPR_UNIT_TEST (sustained_snow_load_converges_without_inventory_loss) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, 64u, 64u, true, nullptr, 9.81f / (64.0f / 3.0f));

        // A fixed-width bin distinguishes axial compaction from lateral spread.
        // Half-cell samples initially occupy a 6-by-8 column above y=12.
        const std::array<GpuStaticShape, 3u> shapes{{
            {.m_position_x = 32.0f, .m_position_y = 10.0f, .m_half_size_x = 10.0f, .m_half_size_y = 2.0f},
            {.m_position_x = 28.0f, .m_position_y = 24.0f, .m_half_size_x = 1.0f, .m_half_size_y = 12.0f},
            {.m_position_x = 36.0f, .m_position_y = 24.0f, .m_half_size_x = 1.0f, .m_half_size_y = 12.0f},
        }};
        GpuMaterialProperties property{};
        property.m_substance_id = 1u;
        property.m_family = ESourceMaterial::compacting;
        property.m_reference_density = 0.3f;

        Array<GpuParticle> particles;
        for (u32 y = 0u; y < 16u; ++y) {
            for (u32 x = 0u; x < 12u; ++x) {
                auto p = restParticle();
                p.m_position_x = 29.25f + 0.5f * float(x);
                p.m_position_y = 12.25f + 0.5f * float(y);
                p.m_material = static_cast<float>(property.m_family);
                p.m_log_jp = 0.0f;
                p.m_volume = 0.25f;
                p.m_mass = 0.075f;
                particles.push_back(p);
            }
        }

        const Array<u32> indices(particles.size(), 0u);
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            particles, shapes, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace,
            {{&property, 1u}, indices}));
        fixture.m_solver.publishReplacement();

        constexpr std::array<u32, 5u> horizons{256u, 512u, 1024u, 2048u, 4096u};
        std::array<double, horizons.size()> heights{};
        std::array<double, horizons.size()> fractions{};
        auto previous = particles;
        u32 completed = 0u;
        for (std::size_t horizon = 0u; horizon < horizons.size(); ++horizon) {
            while (completed < horizons[horizon]) {
                PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(
                    fixture.m_solver.accounting().m_epoch, kGameSubsteps, kDefaultIterations));
                PPR_TEST_ASSERT(not fixture.m_solver.drain());

                const auto complete = fixture.m_solver.pollTick();
                PPR_TEST_ASSERT(complete and *complete);

                fixture.m_solver.publishTick();
                completed += kGameSubsteps;
            }

            auto state = fixture.snapshot();
            PPR_TEST_ASSERT(state.size() == particles.size());
            double bottom = 64.0, top = 0.0, mean_phi = 0.0;
            for (std::size_t row = 0u; row < state.size(); ++row) {
                const auto &p = state[row];
                for (const float value: std::bit_cast<std::array<float, 24u>>(p))
                    PPR_TEST_ASSERT(std::isfinite(value));

                PPR_TEST_ASSERT(validMaterialState(p, property));
                PPR_TEST_ASSERT(p.m_mass == particles[row].m_mass and p.m_volume == particles[row].m_volume);
                PPR_TEST_ASSERT(p.m_solid_fraction >= previous[row].m_solid_fraction);

                bottom = std::min(bottom, static_cast<double>(p.m_position_y));
                top = std::max(top, static_cast<double>(p.m_position_y));
                mean_phi += static_cast<double>(p.m_solid_fraction) / static_cast<double>(state.size());
            }

            heights[horizon] = top - bottom;
            fractions[horizon] = mean_phi;
            std::printf(
                "SNOW-SATURATION substeps=%u height=%.9g mean_phi=%.9g phi_limit=%.9g inventory=exact\n", completed,
                heights[horizon], mean_phi, static_cast<double>(1.0f - property.m_dense_epsilon));
            previous = std::move(state);
        }

        // Height convergence is settling toward a packing limit; per-particle
        // volume is bounded by the porosity cap. Occupied-volume transport across
        // particles is B2's scope. Reference volume and mass remain exact above.
        // The 0.9 factor is a fixture packing allowance for centre-span and boundary
        // sampling, not a physical constant or permission for inventory loss.
        constexpr double initial_height = 7.5;
        const double phi_dense = static_cast<double>(1.0f - property.m_dense_epsilon);
        const double dense_height = initial_height * static_cast<double>(property.m_solid_fraction0) / phi_dense;
        const double packing_floor = 0.9 * dense_height;
        const double height_difference = std::abs(heights[4u] - heights[3u]);
        const double previous_difference = std::abs(heights[3u] - heights[2u]);
        const double height_rate = height_difference / static_cast<double>(horizons[4u] - horizons[3u]);
        const double previous_rate = previous_difference / static_cast<double>(horizons[3u] - horizons[2u]);

        std::printf(
            "SNOW-CONVERGENCE initial=%.9g height2048=%.9g height4096=%.9g difference=%.9g previous_difference=%.9g "
            "rate_per_substep=%.9g previous_rate_per_substep=%.9g "
            "predicted_dense_height=%.9g packing_floor=%.9g final_mean_phi=%.9g\n",
            initial_height, heights[3u], heights[4u], height_difference, previous_difference, height_rate,
            previous_rate, dense_height, packing_floor, fractions.back());

        for (std::size_t horizon = 0u; horizon < horizons.size(); ++horizon) {
            const double preceding_height = horizon == 0u ? initial_height : heights[horizon - 1u];
            const double preceding_fraction = horizon == 0u ? static_cast<double>(property.m_solid_fraction0)
                                                            : fractions[horizon - 1u];
            PPR_TEST_ASSERT(heights[horizon] <= preceding_height);
            PPR_TEST_ASSERT(heights[horizon] >= packing_floor);
            PPR_TEST_ASSERT(fractions[horizon] >= preceding_fraction);
            PPR_TEST_ASSERT(fractions[horizon] <= phi_dense);
        }

        // Normalize by interval length: the last doubling horizon spans twice
        // as many substeps, so settling requires a decreasing per-substep rate.
        PPR_TEST_ASSERT(height_rate < previous_rate);
    };

    PPR_UNIT_TEST (calibrated_loaded_clay_and_snow_diagnostic) {
        const char *const setting = std::getenv("PPR_PBMPM_COST_DIAGNOSTIC");
        const std::string_view enabled = setting != nullptr ? setting : "0";
        PPR_TEST_ASSERT(enabled == "0" or enabled == "1");
        if (enabled != "1") {
            _.log("CALIBRATED-LOADED skipped: set PPR_PBMPM_COST_DIAGNOSTIC=1");
            return;
        }

        runCalibratedLoadedScene(true);
    };

    // ------------------------------------------------------------------
    // initial-velocity excitation and subtractive-kick diagnostic
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (elastic_one_percent_shear_release_diagnostic) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 64u, 64u, true, nullptr, 0.0f);

        Array<GpuParticle> driven;
        for (u32 y = 0u; y < 4u; ++y) {
            for (u32 x = 0u; x < 4u; ++x) {
                auto p = restParticle();
                p.m_material = 1.0f;
                p.m_position_x = 24.5f + float(x);
                p.m_position_y = 24.5f + float(y);
                // Solver velocities are displacements per dt. Free affine
                // transport would accumulate 1% shear over 120 free substeps;
                // this initial-velocity excitation is not a maintained drive.
                p.m_displacement_x = (0.01f / 120.0f) * (float(y) - 1.5f);
                p.m_deformation_displacement = {0.0f, 0.0f, 0.01f / 120.0f, 0.0f};
                driven.push_back(p);
            }
        }

        const auto measure = [&](const std::span<const GpuParticle> state) {
            PPR_TEST_ASSERT(state.size() == driven.size());
            double cx = 0.0, cy = 0.0;
            for (const auto &p: state) {
                cx += static_cast<double>(p.m_position_x);
                cy += static_cast<double>(p.m_position_y);
            }

            cx /= static_cast<double>(state.size());
            cy /= static_cast<double>(state.size());
            double dot = 0.0, cross = 0.0, radius_squared = 0.0;
            for (std::size_t i = 0u; i < state.size(); ++i) {
                const double rx = static_cast<double>(driven[i].m_position_x) - 26.0;
                const double ry = static_cast<double>(driven[i].m_position_y) - 26.0;
                const double x = static_cast<double>(state[i].m_position_x) - cx;
                const double y = static_cast<double>(state[i].m_position_y) - cy;
                dot += rx * x + ry * y;
                cross += rx * y - ry * x;
                radius_squared += rx * rx + ry * ry;
            }

            const double angle = std::atan2(cross, dot);
            const double c = std::cos(angle), s = std::sin(angle);
            double error_squared = 0.0, residual = 0.0;
            for (std::size_t i = 0u; i < state.size(); ++i) {
                const double rx = static_cast<double>(driven[i].m_position_x) - 26.0;
                const double ry = static_cast<double>(driven[i].m_position_y) - 26.0;
                const double dx = static_cast<double>(state[i].m_position_x) - cx - (c * rx - s * ry);
                const double dy = static_cast<double>(state[i].m_position_y) - cy - (s * rx + c * ry);
                error_squared += dx * dx + dy * dy;
            }

            for (const auto &p: state) {
                const auto &f = p.m_deformation_gradient;
                // F^T F removes rigid rotation; translation never enters F.
                const double c0x = static_cast<double>(f.m_c0x), c0y = static_cast<double>(f.m_c0y);
                const double c1x = static_cast<double>(f.m_c1x), c1y = static_cast<double>(f.m_c1y);
                const double a = c0x * c0x + c0y * c0y - 1.0;
                const double b = c1x * c1x + c1y * c1y - 1.0;
                const double off = c0x * c1x + c0y * c1y;
                residual = std::max(residual, std::sqrt(a * a + b * b + 2.0 * off * off));
            }

            return std::array<double, 2u>{residual, std::sqrt(error_squared / radius_squared)};
        };

        fixture.replace(driven);
        auto state = driven;
        std::array<double, 2u> peak{}, final{}, tail_max{};
        for (u32 step = 1u; step <= 120u; ++step) {
            fixture.finishOneSubstep(kDefaultIterations);
            state = fixture.publishCompletedTick();
            final = measure(state);
            for (u32 metric = 0u; metric < 2u; ++metric)
                peak[metric] = std::max(peak[metric], final[metric]);

            std::printf("B1 elastic phase=drive step=%u objective=%.9g aligned_rms=%.9g\n", step, final[0u], final[1u]);
        }

        // Apply a subtractive kick equal to the initial affine contribution.
        // Nonlinear free evolution does not preserve an independently removable
        // driving component. The residual is a diagnostic lower/upper bound,
        // not an exact recovery metric; geometry and F remain unchanged.
        for (std::size_t i = 0u; i < state.size(); ++i) {
            state[i].m_displacement_x -= driven[i].m_displacement_x;
            state[i].m_deformation_displacement.m_c1x -= 0.01f / 120.0f;
        }

        fixture.replace(state);
        bool sustained = true;
        for (u32 step = 1u; step <= 960u; ++step) {
            fixture.finishOneSubstep(kDefaultIterations);
            state = fixture.publishCompletedTick();
            final = measure(state);
            if (step > 720u) {
                sustained = sustained and final[0u] < 1e-3 and final[1u] < 1e-3;
                for (u32 metric = 0u; metric < 2u; ++metric)
                    tail_max[metric] = std::max(tail_max[metric], final[metric]);
            }

            std::printf(
                "B1 elastic phase=release step=%u objective=%.9g aligned_rms=%.9g\n", step, final[0u], final[1u]);
        }

        // An excitation that never exceeds the recovery thresholds cannot establish
        // recovery. Report achieved excitation rather than assuming 1% was held.
        const bool excited = peak[0u] >= 1e-3 and peak[1u] >= 1e-3;
        std::printf(
            "B1 elastic diagnostic K=%u drive_s=0.25 release_s=2 peak_objective=%.9g peak_rms=%.9g final_objective=%.9g final_rms=%.9g tail_max_objective=%.9g tail_max_rms=%.9g verdict=%s rock_ore=granular\n",
            kDefaultIterations, peak[0u], peak[1u], final[0u], final[1u], tail_max[0u], tail_max[1u],
            excited and sustained ? "sustained_recovery" : "not_established");
        // A negative recovery verdict is evidence, not a failing test or remap.
    };

    PPR_UNIT_TEST (substance_rows_preserve_defaults_and_reject_invalid_replacements) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);

        std::array<GpuParticle, 4u> particles{};
        std::array<GpuMaterialProperties, 4u> rows{};
        std::array<u32, 4u> indices{0u, 1u, 2u, 3u};
        for (u32 family = 0u; family < 4u; ++family) {
            particles[family] = restParticle();
            particles[family].m_position_x += 6.0f * static_cast<float>(family);
            particles[family].m_material = static_cast<float>(family);
            particles[family].m_liquid_density = 0.8f;
            particles[family].m_deformation_displacement = {0.0f, 0.02f, 0.02f, 0.0f};
            rows[family].m_substance_id = family + 1u;
            rows[family].m_family = static_cast<ESourceMaterial>(family);
        }

        for (const u32 iterations: kAdmittedIterations) {
            fixture.replace(particles);
            fixture.finishOneSubstep(iterations);
            const auto baseline = fixture.publishCompletedTick();
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                particles, {}, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace, {rows, indices}));
            fixture.m_solver.publishReplacement();
            fixture.finishOneSubstep(iterations);
            const auto mapped = fixture.publishCompletedTick();
            PPR_TEST_ASSERT(std::memcmp(baseline.data(), mapped.data(), sizeof(particles)) == 0);
        }

        const auto before = fixture.snapshot();
        const auto epoch = fixture.m_solver.accounting().m_epoch;
        for (u32 invalid = 0u; invalid < 8u; ++invalid) {
            auto rejected_rows = rows;
            auto rejected_indices = indices;
            if (invalid == 0u)
                rejected_rows[0u].m_substance_id = 0u;
            if (invalid == 1u)
                rejected_rows[1u].m_substance_id = rejected_rows[0u].m_substance_id;
            if (invalid == 2u)
                rejected_rows[0u].m_friction_angle = 90.0f;
            if (invalid == 3u)
                rejected_rows[0u].m_plasticity = -1000.0f;
            if (invalid == 4u)
                rejected_rows[0u].m_liquid_viscosity = std::numeric_limits<float>::infinity();
            if (invalid == 5u)
                rejected_rows[0u].m_family = static_cast<ESourceMaterial>(6u);
            if (invalid == 6u)
                rejected_rows[0u].m_reference_density = 0.0f;
            if (invalid == 7u)
                rejected_indices[0u] = 4u;

            PPR_TEST_ASSERT(
                fixture.m_solver.prepareReplacement(
                    particles, {}, epoch, {}, ETerrainUpdate::replace, {rejected_rows, rejected_indices}) ==
                std::errc::invalid_argument);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch and not fixture.m_solver.busy());
            const auto unchanged = fixture.snapshot();
            PPR_TEST_ASSERT(std::memcmp(before.data(), unchanged.data(), sizeof(particles)) == 0);
        }

        fixture.finishOneSubstep();
        const auto resumed = fixture.publishCompletedTick();
        PPR_TEST_ASSERT(resumed.size() == particles.size());

        const auto retained_epoch = fixture.m_solver.accounting().m_epoch;
        auto changed_prefix = rows;
        changed_prefix[0u].m_liquid_viscosity = 0.25f;
        PPR_TEST_ASSERT(
            fixture.m_solver.prepareAppend({}, retained_epoch, {}, ETerrainUpdate::preserve, {changed_prefix, {}}) ==
            std::errc::invalid_argument);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == retained_epoch);

        // Start the append probe with two distinguishable rows of the same family.
        // Semantic snapshots omit propertyRow, so inspect the committed cold buffer.
        std::array<GpuMaterialProperties, 3u> growing_rows{};
        for (u32 index = 0u; index < growing_rows.size(); ++index) {
            growing_rows[index].m_substance_id = 11u + index;
            growing_rows[index].m_family = ESourceMaterial::liquid;
            growing_rows[index].m_liquid_viscosity = 0.125f * static_cast<float>(index);
        }

        auto retained_particle = restParticle();
        retained_particle.m_deformation_displacement = {0.0f, 0.1f, 0.1f, 0.0f};
        const std::array<u32, 1u> retained_index{1u};
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            {&retained_particle, 1u}, {}, retained_epoch, {}, ETerrainUpdate::replace,
            {std::span{growing_rows}.first(2u), retained_index}));
        fixture.m_solver.publishReplacement();
        const auto append_epoch = fixture.m_solver.accounting().m_epoch;

        const auto read_index = [&](const u32 particle) {
            const auto view = fixture.m_solver.committedView();
            PPR_TEST_ASSERT(view and particle < view->m_count);

            ParticleReadonly cold{};
            PPR_TEST_ASSERT(not make_error_code(fixture.m_rhi->getDevice().readBuffer(
                view->m_cold, particle * sizeof(ParticleReadonly), sizeof(cold), &cold)));
            return cold.m_property_row;
        };

        PPR_TEST_ASSERT(read_index(0u) == 1u);

        auto appended = retained_particle;
        appended.m_position_x += 12.0f;
        const std::array<u32, 1u> appended_index{2u};
        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend(
            {&appended, 1u}, append_epoch, {}, ETerrainUpdate::preserve, {growing_rows, appended_index}));
        PPR_TEST_ASSERT(not fixture.m_solver.cancelPreparedReplacement());
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == append_epoch);
        PPR_TEST_ASSERT(read_index(0u) == 1u);
        PPR_TEST_ASSERT(growing_rows[read_index(0u)].m_liquid_viscosity == 0.125f);

        const auto cancelled = fixture.snapshot();
        PPR_TEST_ASSERT(cancelled.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(cancelled.data(), &retained_particle, sizeof(GpuParticle)) == 0);

        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend(
            {&appended, 1u}, append_epoch, {}, ETerrainUpdate::preserve, {growing_rows, appended_index}));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto copied = fixture.m_solver.pollReplacement();
        PPR_TEST_ASSERT(copied and *copied);

        fixture.m_solver.publishReplacement();
        const auto appended_state = fixture.snapshot();
        PPR_TEST_ASSERT(appended_state.size() == 2u);
        PPR_TEST_ASSERT(std::memcmp(appended_state.data(), &retained_particle, sizeof(GpuParticle)) == 0);
        PPR_TEST_ASSERT(read_index(0u) == 1u and read_index(1u) == 2u);
        PPR_TEST_ASSERT(growing_rows[read_index(0u)].m_liquid_viscosity == 0.125f);

        // Cancellation must also retain the already-published extended table.
        std::array<GpuMaterialProperties, 4u> cancelled_rows{};
        std::copy(growing_rows.begin(), growing_rows.end(), cancelled_rows.begin());
        cancelled_rows[3u] = growing_rows[0u];
        cancelled_rows[3u].m_substance_id = 14u;
        cancelled_rows[3u].m_liquid_viscosity = 0.5f;
        const std::array<u32, 1u> cancelled_index{3u};
        const auto published_append_epoch = fixture.m_solver.accounting().m_epoch;
        PPR_TEST_ASSERT(not fixture.m_solver.prepareAppend(
            {&appended, 1u}, published_append_epoch, {}, ETerrainUpdate::preserve, {cancelled_rows, cancelled_index}));
        PPR_TEST_ASSERT(not fixture.m_solver.cancelPreparedReplacement());
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == published_append_epoch);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_particle_count == 2u);
        PPR_TEST_ASSERT(read_index(0u) == 1u and read_index(1u) == 2u);
        PPR_TEST_ASSERT(growing_rows[read_index(0u)].m_liquid_viscosity == 0.125f);

        fixture.finishOneSubstep();
        const auto append_result = fixture.publishCompletedTick();

        // Replacement rebuilds indices against a permuted table. Both particles
        // must still consume exactly the same coefficients as the append path.
        auto replacement_rows = growing_rows;
        std::swap(replacement_rows[0u], replacement_rows[1u]);
        const std::array<u32, 2u> rebuilt_indices{0u, 2u};
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            appended_state, {}, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace,
            {replacement_rows, rebuilt_indices}));
        fixture.m_solver.publishReplacement();
        PPR_TEST_ASSERT(read_index(0u) == 0u);
        PPR_TEST_ASSERT(replacement_rows[read_index(0u)].m_liquid_viscosity == 0.125f);

        fixture.finishOneSubstep();
        const auto replacement_result = fixture.publishCompletedTick();
        PPR_TEST_ASSERT(append_result.size() == replacement_result.size());
        PPR_TEST_ASSERT(
            std::memcmp(append_result.data(), replacement_result.data(), append_result.size() * sizeof(GpuParticle)) ==
            0);
    };

    PPR_UNIT_TEST (device_property_guard_reports_out_of_range_and_family_mismatch) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        shader::SharedModule module;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "property_checks.slang", "pbmpm_property_checks",
            module.writeRef()));
        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_TEST_ASSERT(
            not make_error_code(module->findEntryPointByName("inspectPropertyAdmission", entry.writeRef())));

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

        // Bypass host admission deliberately. The first table is too short for
        // index 1; the second has a corrupted family at the valid index 0.
        for (const u32 field: {0u, 1u}) {
            ParticleReadonly cold = packReadonly(restParticle());
            cold.m_property_row = field == 0u ? 1u : 0u;

            GpuMaterialProperties properties{};
            properties.m_substance_id = 11u;
            properties.m_family = field == 0u ? ESourceMaterial::liquid : ESourceMaterial::sand;
            const std::array<u32, 6u> initial_errors{0u, 0u, 0xffff'ffffu, 0xffff'ffffu, 0xffff'ffffu, 0xffff'ffffu};

            const auto buffer = [&](const void *const data, const u64 size, const u32 stride, const bool writable) {
                rhi::BufferDesc desc{};
                desc.size = size;
                desc.elementSize = stride;
                desc.usage = writable ? static_cast<rhi::BufferUsage>(
                                            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                                            static_cast<u32>(rhi::BufferUsage::CopySource))
                                      : rhi::BufferUsage::ShaderResource;
                desc.defaultState = writable ? rhi::ResourceState::UnorderedAccess : rhi::ResourceState::ShaderResource;
                rhi::ComPtr<rhi::IBuffer> result;
                PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, data, result.writeRef())));
                return result;
            };

            const auto cold_buffer = buffer(&cold, sizeof(cold), sizeof(cold), false);
            const auto table_buffer = buffer(&properties, sizeof(properties), sizeof(properties), false);
            const auto errors = buffer(initial_errors.data(), sizeof(initial_errors), sizeof(u32), true);

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            auto *const root = pass->bindPipeline(pipeline);
            PPR_TEST_ASSERT(root != nullptr);
            rhi::ShaderCursor cursor{root};
            PPR_TEST_ASSERT(not make_error_code(
                cursor["g_cold"].setBinding(rhi::Binding(cold_buffer, {.offset = 0u, .size = sizeof(cold)}))));
            PPR_TEST_ASSERT(not make_error_code(
                cursor["g_properties"].setBinding(
                    rhi::Binding(table_buffer, {.offset = 0u, .size = sizeof(properties)}))));
            PPR_TEST_ASSERT(not make_error_code(
                cursor["g_errors"].setBinding(rhi::Binding(errors, {.offset = 0u, .size = sizeof(initial_errors)}))));
            pass->dispatchCompute(1u, 1u, 1u);
            pass->end();

            rhi::ComPtr<rhi::ICommandBuffer> commands;
            PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
            PPR_DEFER {
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
            };
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

            std::array<u32, 6u> actual{};
            PPR_TEST_ASSERT(not make_error_code(device.readBuffer(errors, 0u, sizeof(actual), actual.data())));
            PPR_TEST_ASSERT(actual[0u] == 512u and actual[1u] == 1u);
            PPR_TEST_ASSERT(actual[2u] == cold.m_property_row and actual[3u] == field);
            PPR_TEST_ASSERT(actual[4u] == 0u and actual[5u] == 0xffff'ffffu);
        }
    };

    PPR_UNIT_TEST (same_family_substances_differ_and_unused_rows_do_not_dispatch) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);
        std::array<GpuParticle, 2u> particles{restParticle(), restParticle()};
        particles[1u].m_position_x += 12.0f;
        for (auto &particle: particles) {
            particle.m_deformation_displacement = {0.0f, 0.1f, 0.1f, 0.0f};
        }

        Array<GpuMaterialProperties> rows(2u);
        rows[0u].m_substance_id = 11u;
        rows[1u].m_substance_id = 12u;
        rows[0u].m_family = rows[1u].m_family = ESourceMaterial::liquid;
        rows[0u].m_liquid_viscosity = 0.0f;
        rows[1u].m_liquid_viscosity = 0.5f;
        std::array<u32, 2u> indices{0u, 1u};
        const auto run = [&]() {
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                particles, {}, fixture.m_solver.accounting().m_epoch, {}, ETerrainUpdate::replace, {rows, indices}));
            fixture.m_solver.publishReplacement();
            const u64 dispatches = fixture.m_solver.accounting().m_dispatches;
            fixture.finishOneSubstep();
            auto result = fixture.publishCompletedTick();
            return std::pair{std::move(result), fixture.m_solver.accounting().m_dispatches - dispatches};
        };

        const auto baseline = run();
        PPR_TEST_ASSERT(
            std::abs(
                baseline.first[0u].m_deformation_displacement.m_c0y -
                baseline.first[1u].m_deformation_displacement.m_c0y) > 0.01f);

        GpuMaterialProperties unused{};
        unused.m_substance_id = 99u;
        unused.m_family = ESourceMaterial::elastic;
        rows.push_back(unused);
        const auto extended = run();
        PPR_TEST_ASSERT(extended.second == baseline.second);
        PPR_TEST_ASSERT(std::memcmp(extended.first.data(), baseline.first.data(), sizeof(particles)) == 0);

        std::swap(rows[0u], rows[1u]);
        std::swap(indices[0u], indices[1u]);
        std::swap(rows[0u].m_substance_id, rows[1u].m_substance_id);
        const auto reordered = run();
        PPR_TEST_ASSERT(reordered.second == baseline.second);
        PPR_TEST_ASSERT(std::memcmp(reordered.first.data(), baseline.first.data(), sizeof(particles)) == 0);
    };

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
            const auto scalars = std::bit_cast<std::array<float, 24u>>(result[row]);
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

    // ------------------------------------------------------------------
    // retained Phase A sand-law characterization
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (sand_history_phase_a_characterization) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_gravity_strength == 0.0f);

        std::array<GpuParticle, 2u> particles{restParticle(), restParticle()};
        for (u32 row = 0u; row < particles.size(); ++row) {
            particles[row].m_material = static_cast<float>(ESourceMaterial::sand);
            particles[row].m_position_x = 6.5f + 14.0f * static_cast<float>(row);
            particles[row].m_log_jp = row == 0u ? 0.2f : 1.0f;
        }

        // Isolated, motionless rows: F=I, D=0, no gravity, brush or contacts.
        // H is a hot-row field and never crosses the fixed-point grid. Both
        // scaling factors (1/2 and 2^-n) are powers of two: halving the stored
        // FP32 H is exact here, without underflow. 0.2f itself is NOT a power
        // of two; the reference starts from that same rounded input value.
        for (const u32 iterations: {2u, 5u, 10u}) {
            fixture.replace(particles);
            for (u32 substep = 1u; substep <= 8u; ++substep) {
                const u64 epoch = fixture.m_solver.accounting().m_epoch;
                fixture.finishOneSubstep(iterations);
                PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);

                const auto state = fixture.publishCompletedTick();
                PPR_TEST_ASSERT(state.size() == particles.size());
                // The observable advancement event is committed publication:
                // one epoch per tick, not one per solver stage.
                PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
                PPR_TEST_ASSERT(fixture.m_solver.accounting().m_substeps == 1u);
                for (u32 row = 0u; row < state.size(); ++row) {
                    const float expected = std::ldexp(particles[row].m_log_jp, -static_cast<int>(substep));
                    PPR_TEST_ASSERT(state[row].m_log_jp == expected);

                    // eHat = log(Sigma) - (sum(log(Sigma)) + H)/2 has
                    // trace -H, including at F=I. eHat is not observable here;
                    // only the committed pair (F approximately I, H=H0*2^-n)
                    // is asserted. It exposes ledger loss at rest, not a direct
                    // measurement of eHat or proof of a physically correct law.
                    // F can consume grid-derived D: retain the schedule leaf's
                    // n*1e-5 allowance (9 nodes * 9 gathers / 1e7 < 1e-5).
                    const auto &f = state[row].m_deformation_gradient;
                    const float bound = static_cast<float>(substep) * 1.0e-5f;
                    PPR_TEST_ASSERT(std::abs(f.m_c0x - 1.0f) <= bound);
                    PPR_TEST_ASSERT(std::abs(f.m_c0y) <= bound);
                    PPR_TEST_ASSERT(std::abs(f.m_c1x) <= bound);
                    PPR_TEST_ASSERT(std::abs(f.m_c1y - 1.0f) <= bound);
                }
            }

            // A multi-substep tick separates history updates from publication:
            // three halvings, but still only one committed advancement event.
            fixture.replace(particles);
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 3u, iterations));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());

            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);

            const auto state = fixture.publishCompletedTick();
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_substeps == 3u);
            PPR_TEST_ASSERT(state.size() == particles.size());
            for (u32 row = 0u; row < state.size(); ++row) {
                PPR_TEST_ASSERT(state[row].m_log_jp == particles[row].m_log_jp * 0.125f);
            }
        }

        // Positive-history anisotropic compression, away from branch boundaries.
        // With elasticityRatio=1 and H>0 the constraint target is F itself;
        // diag(1, 1/2) also makes its inverse/reconstruction dyadic at D=0.
        // Current return: e' = e - a*eHat, H'=0, trace(e')=s+a*H.
        // The rejected candidate instead gives trace(e')=t=s+H, H'=0
        // and e'=(t/2 +/- min(n_dev,-c*t)/sqrt(2)), a different F.
        GpuParticle anisotropic = particles[0u];
        anisotropic.m_deformation_gradient = {1.0f, 0.0f, 0.0f, 0.5f};
        const auto profile = fixture.m_solver.profile();
        PPR_TEST_ASSERT(profile.m_elasticity_ratio == 1.0f);
        const float s = std::log(0.5f);
        const float t = s + anisotropic.m_log_jp;
        const float hat_x = -0.5f * t;
        const float hat_y = s - 0.5f * t;
        const float norm = std::sqrt(hat_x * hat_x + hat_y * hat_y);
        const float sin_phi = std::sin(profile.m_friction_angle / 180.0f * 3.14159f);
        const float c = 2.0f * std::sqrt(2.0f / 3.0f) * 2.0f * sin_phi / (3.0f - sin_phi);
        const float a = (norm + c * t) / norm;
        PPR_TEST_ASSERT(t < 0.0f and a > 0.0f and a < 1.0f);

        for (const u32 iterations: {2u, 5u, 10u}) {
            fixture.replace({&anisotropic, 1u});
            fixture.finishOneSubstep(iterations);
            const auto state = fixture.publishCompletedTick();
            PPR_TEST_ASSERT(state.size() == 1u);
            PPR_TEST_ASSERT(state[0u].m_log_jp == 0.0f);

            // 2e-5 covers the <=8.1e-6 grid-transfer scale plus FP32
            // SVD/log/exp reconstruction for this bounded, unclipped fixture.
            constexpr float kProjectionTolerance = 2.0e-5f;
            const auto &f = state[0u].m_deformation_gradient;
            PPR_TEST_ASSERT(std::abs(f.m_c0x - std::exp(-a * hat_x)) <= kProjectionTolerance);
            PPR_TEST_ASSERT(std::abs(f.m_c1y - std::exp(s - a * hat_y)) <= kProjectionTolerance);
            PPR_TEST_ASSERT(std::abs(f.m_c0y) <= kProjectionTolerance);
            PPR_TEST_ASSERT(std::abs(f.m_c1x) <= kProjectionTolerance);

            const float ledger = std::log(f.m_c0x * f.m_c1y - f.m_c0y * f.m_c1x) + state[0u].m_log_jp;
            PPR_TEST_ASSERT(std::abs(ledger - (s + a * anisotropic.m_log_jp)) <= kProjectionTolerance);
            PPR_TEST_ASSERT(std::abs(ledger - t) > 100.0f * kProjectionTolerance);
        }
    };

    // ------------------------------------------------------------------
    // admitted iteration schedule
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (supported_iteration_counts_validate_before_work) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        const GpuParticle particle = restParticle();
        fixture.replace({&particle, 1u});
        const GpuAccounting before = fixture.m_solver.accounting();

        for (const u32 iterations: {0u, 1u, 3u, 4u, std::numeric_limits<u32>::max()}) {
            PPR_TEST_ASSERT(
                fixture.m_solver.submitResidentTick(before.m_epoch, 1u, iterations) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_dispatches == before.m_dispatches);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == before.m_epoch);
        }

        const auto unchanged = fixture.snapshot();
        PPR_TEST_ASSERT(unchanged.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(unchanged.data(), &particle, sizeof(GpuParticle)) == 0);

        for (const u32 iterations: {2u, 5u, 10u}) {
            // Each admitted count owns one complete submit -> drain -> poll ->
            // publish sequence; nothing is submitted while the previous tick is
            // still pending.
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            const u64 dispatches = fixture.m_solver.accounting().m_dispatches;
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, iterations));

            PPR_TEST_ASSERT(not fixture.m_solver.drain());

            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
            // Nine maintenance dispatches around discovery, then one destination
            // initialization and one fused dispatch per stage.
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_dispatches - dispatches == 9u + 2u * iterations);

            const auto state = fixture.publishCompletedTick();
            PPR_TEST_ASSERT(state.size() == 1u);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 1u);
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
        }
    };

    PPR_UNIT_TEST (iteration_roles_integrate_once_per_substep) {
        // The observable integration contract is one *gravity step* per substep,
        // not an exact delta: a substep's published displacement is the gathered
        // grid displacement plus that one step, and the gather reconstructs the
        // previous displacement through the fixed-point transfer. Momentum and
        // mass cross the grid as int(value * 1e7), so a stage moves the
        // reconstructed velocity by at most one count per contributing node:
        // with unit mass and dyadic quadratic weights (1/16, 1/8, 1/4) the mass
        // word is exact, so the weighted error of the nine-node stencil is at most
        // 9 / (mass * 1e7) = 9e-7 per stage, i.e. 0.26 % of this profile's
        // |gravity| = 3.472e-4. A K stage substep re-encodes at most K-1 = 9
        // gather stages, so the coherent worst case is 2.3 % of |gravity|.
        // 5 % therefore leaves a 2x margin over the transfer's own quantization
        // while staying 20x away from two integrations (2 * gravity) and further
        // from K of them, which is the only failure this leaf must catch.
        constexpr float kOneIntegrationFraction = 0.05f;

        // Per-substep allowance for one committed deformation-gradient component,
        // derived beside its use below: 9 stencil nodes * 9 gather re-encodes *
        // 1/(mass * 1e7) = 8.1e-6, rounded up to 1e-5.
        constexpr float kOneGradientStepBound = 1.0e-5f;

        // Relative allowance for one committed plastic-history halving: the additive
        // deviatoric term is below 5e-7 while these histories are 1, 0.5 and 0.25,
        // so 1e-4 * previous keeps 50-200x margin. Derived beside its use below.
        constexpr float kOnePlasticHistoryFraction = 1.0e-4f;

        const auto gravity_after_one_step = [](const GpuParameters &profile) {
            return -static_cast<float>(profile.m_grid_height) *
                   profile.m_gravity_strength *
                   profile.m_delta_time *
                   profile.m_delta_time;
        };

        {
            Fixture fixture;
            PPR_DEFER {
                fixture.shutdown();
            };
            fixture.initialize(false);
            const GpuParameters profile = fixture.m_solver.profile();
            const float gravity = gravity_after_one_step(profile);
            PPR_TEST_ASSERT(gravity < 0.0f);

            // One liquid and one sand row, far enough apart that neither particle's
            // stencil reaches the other: every stage reconstructs only its own state.
            std::array<GpuParticle, 2u> particles{};
            particles[0u] = restParticle();
            particles[1u] = restParticle();
            particles[1u].m_position_x = 20.5f;
            particles[1u].m_material = static_cast<float>(ESourceMaterial::sand);
            particles[1u].m_log_jp = 1.0f;

            for (const u32 iterations: {2u, 5u, 10u}) {
                fixture.replace(particles);
                float previous_displacement_y = particles[0u].m_displacement_y;
                float previous_log_jp = particles[1u].m_log_jp;

                for (u32 substep = 1u; substep <= 3u; ++substep) {
                    fixture.finishOneSubstep(iterations);
                    const auto state = fixture.publishCompletedTick();
                    PPR_TEST_ASSERT(state.size() == particles.size());
                    for (const auto &row: state) {
                        const auto scalars = std::bit_cast<std::array<float, 24u>>(row);
                        for (const float value: scalars) {
                            PPR_TEST_ASSERT(std::isfinite(value));
                        }
                    }

                    // Gravity enters only through integration: exactly one step per
                    // substep, whatever K is. Extra integrations would multiply the
                    // step, and a substep that never integrated would leave it out.
                    const float step = state[0u].m_displacement_y - previous_displacement_y;
                    const float step_ratio = std::abs(step - gravity) / std::abs(gravity);
                    const float cumulative_ratio = std::abs(
                                                       state[0u].m_displacement_y -
                                                       static_cast<float>(substep) * gravity) /
                                                   (static_cast<float>(substep) * std::abs(gravity));

                    // The sand row's committed gradient is written by integration as
                    // the polar factor of (I + D)F, and D is the gathered deformation
                    // displacement, so F carries the transfer's own quantization and
                    // cannot stay bit-exactly identity. One gather moves a
                    // reconstructed component by at most 9/(mass * 1e7) = 9e-7 (nine
                    // stencil nodes, one count each, unit mass); a K-stage substep
                    // re-encodes at most K-1 = 9 gathers, so one substep allows
                    // 9 * 9e-7 = 8.1e-6. The polar remainder survives the next
                    // substep, so substep n allows n times that allowance; 1e-5 per
                    // substep keeps a 1.2x margin over 8.1e-6, while a per-stage
                    // integration loop would exceed it by the K-1 = 9 factor
                    // (7.3e-5 in the first substep alone).
                    const auto &sand_gradient = state[1u].m_deformation_gradient;
                    const float gradient_deviation = std::max(
                        {std::abs(sand_gradient.m_c0x - 1.0f), std::abs(sand_gradient.m_c0y),
                            std::abs(sand_gradient.m_c1x), std::abs(sand_gradient.m_c1y - 1.0f)});
                    const float gradient_bound = static_cast<float>(substep) * kOneGradientStepBound;

                    // Sand commits its plastic history once per substep: integration
                    // computes traceE = sum(log Sigma) + logJp and stores 0.5 * traceE,
                    // so the committed value is half the previous one plus half the
                    // deviatoric term. While F was bit-exactly identity both log
                    // singular values were exactly zero; with the transfer noise above
                    // F deviates by O(1e-7) per component, so each log singular value
                    // is O(1e-7), their sum stays below 1e-6 and the additive term
                    // below 5e-7. A relative allowance of 1e-4 * previous (1e-4 down to
                    // 2.5e-5 across these three substeps) is 50-200x that bound, while
                    // a per-stage integration loop would halve the whole history at
                    // every stage (previous / 2^(K-1)), leaving this allowance already
                    // in the first substep for K > 2.
                    const float log_jp = state[1u].m_log_jp;
                    const float log_jp_difference = log_jp - previous_log_jp * 0.5f;
                    const float log_jp_allowance = previous_log_jp * kOnePlasticHistoryFraction;

                    _.log(
                        std::format(
                            "PBMPM-SCHEDULE K={} substep={} gravity={:.9g} displacement_y={:.9g} "
                            "delta_displacement_y={:.9g} step_ratio={:.6g} cumulative_ratio={:.6g} "
                            "gradient_deviation={:.6g} gradient_bound={:.6g} log_jp={:.9g} "
                            "previous_log_jp={:.9g} log_jp_difference={:.6g} log_jp_allowance={:.6g}",
                            iterations, substep, gravity, state[0u].m_displacement_y, step, step_ratio,
                            cumulative_ratio, gradient_deviation, gradient_bound, log_jp, previous_log_jp,
                            log_jp_difference, log_jp_allowance));
                    PPR_TEST_ASSERT(step_ratio <= kOneIntegrationFraction);
                    PPR_TEST_ASSERT(cumulative_ratio <= kOneIntegrationFraction);
                    PPR_TEST_ASSERT(gradient_deviation <= gradient_bound);
                    PPR_TEST_ASSERT(std::abs(log_jp_difference) <= log_jp_allowance);

                    // One halving per substep, never a frozen or reversed history.
                    PPR_TEST_ASSERT(log_jp < previous_log_jp);

                    PPR_TEST_ASSERT(state[0u].m_displacement_x == 0.0f);
                    PPR_TEST_ASSERT(state[0u].m_position_x == particles[0u].m_position_x);

                    previous_displacement_y = state[0u].m_displacement_y;
                    previous_log_jp = log_jp;
                }
            }
        }

        {
            // Zero-force translation: no gravity and no brush, so no stage may
            // inject acceleration or advance the plastic history.
            Fixture fixture;
            PPR_DEFER {
                fixture.shutdown();
            };
            fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);
            PPR_TEST_ASSERT(fixture.m_solver.profile().m_gravity_strength == 0.0f);
            const GpuParticle particle = restParticle();

            for (const u32 iterations: {2u, 5u, 10u}) {
                fixture.replace({&particle, 1u});

                for (u32 substep = 1u; substep <= 2u; ++substep) {
                    fixture.finishOneSubstep(iterations);
                    const auto state = fixture.publishCompletedTick();
                    PPR_TEST_ASSERT(state.size() == 1u);
                    PPR_TEST_ASSERT(
                        std::bit_cast<std::array<float, 24u>>(state[0u]) ==
                        std::bit_cast<std::array<float, 24u>>(particle));
                }
            }
        }
    };

    PPR_UNIT_TEST (odd_even_schedule_preserves_sparse_sources) {
        // Same contract as iteration_roles_integrate_once_per_substep: one gravity
        // step per substep, measured as a fraction of gravity because the fixed-
        // point transfer's own quantization (at most 9e-7 per stage, 2.3 % of
        // |gravity| for a K=10 substep) is not an integration count. 5 % separates
        // n steps from n/2 or n/4 steps by a wide margin in either direction.
        constexpr float kOneIntegrationFraction = 0.05f;

        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        const GpuParameters profile = fixture.m_solver.profile();
        const float gravity = -static_cast<float>(profile.m_grid_height) *
                              profile.m_gravity_strength *
                              profile.m_delta_time *
                              profile.m_delta_time;
        PPR_TEST_ASSERT(gravity < 0.0f);

        const GpuParticle particle = restParticle();
        const auto runTick = [&](const u32 substeps, const u32 iterations) {
            // One unbroken sequence: a pending tick owns the solver until it is
            // published, so prove that before submitting the next batch.
            PPR_TEST_ASSERT(not fixture.m_solver.busy());
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, substeps, iterations));

            PPR_TEST_ASSERT(not fixture.m_solver.drain());

            const auto complete = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(complete and *complete);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
            return fixture.publishCompletedTick();
        };

        for (const u32 iterations: {2u, 5u, 10u}) {
            for (const u32 substeps: {1u, 2u, 8u}) {
                fixture.replace({&particle, 1u});
                const auto first = runTick(substeps, iterations);
                PPR_TEST_ASSERT(first.size() == 1u);
                const auto scalars = std::bit_cast<std::array<float, 24u>>(first[0u]);
                for (const float value: scalars) {
                    PPR_TEST_ASSERT(std::isfinite(value));
                }

                // Every substep after the first reconstructs the state the
                // previous substep wrote, on whichever side K parity left it, so
                // n substeps must carry n gravity steps and not fewer.
                const float expected = static_cast<float>(substeps) * gravity;
                const float ratio = std::abs(first[0u].m_displacement_y - expected) / std::abs(expected);
                _.log(
                    std::format(
                        "PBMPM-SCHEDULE-SOURCE K={} substeps={} gravity={:.9g} displacement_y={:.9g} "
                        "expected={:.9g} cumulative_ratio={:.6g}",
                        iterations, substeps, gravity, first[0u].m_displacement_y, expected, ratio));
                PPR_TEST_ASSERT(ratio <= kOneIntegrationFraction);
                PPR_TEST_ASSERT(first[0u].m_displacement_x == 0.0f);

                // The same seed and schedule on the same backend reproduce the row
                // exactly, so no side carries state across ticks.
                fixture.replace({&particle, 1u});
                const auto second = runTick(substeps, iterations);
                PPR_TEST_ASSERT(second.size() == first.size());
                PPR_TEST_ASSERT(std::memcmp(second.data(), first.data(), first.size() * sizeof(GpuParticle)) == 0);
            }
        }

        // The side the schedule finished on is not republished, so its index and
        // payload stay readable at the generation it was written with. Poisoning
        // the next substep's destination proves that read cannot consume it.
        {
            SparseHarness harness{fixture, 4u};
            const auto &buffers = harness.m_storage.buffers();
            const GpuParticle row = restParticle();
            harness.rows({&row, 1u});

            Array<u32> dense(18u * 18u * 4u);
            for (u32 word = 0u; word < dense.size(); ++word) {
                dense[word] = word * 4u + 1u;
            }

            const std::array<std::array<i32, 2u>, 2u> nodes{{{3, 3}, {6, 6}}};
            u32 first_generation{};
            harness.run([&](rhi::ICommandEncoder &encoder) {
                PPR_TEST_ASSERT(not harness.m_storage.beginTick(encoder, 2u));
                harness.discover(encoder, false);
                first_generation = harness.m_storage.constants().m_generation;
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, false, harness.m_roots));
                harness.seedDense(encoder, dense);
                PPR_TEST_ASSERT(not harness.m_storage.initializeDestination(encoder, true, harness.m_roots));

                Array<u32> poison(buffers.m_payload[0u]->getDesc().size / sizeof(u32), 0x13579bdfu);
                encoder.setBufferState(buffers.m_payload[0u], rhi::ResourceState::CopyDestination);
                PPR_TEST_ASSERT(not make_error_code(encoder.uploadBufferData(
                    buffers.m_payload[0u], 0u, buffers.m_payload[0u]->getDesc().size, poison.data())));
                encoder.setBufferState(buffers.m_payload[0u], rhi::ResourceState::UnorderedAccess);
                encoder.globalBarrier();

                // Substep two finished on B and its first stage therefore writes A.
                harness.discover(encoder, true);
                harness.probe(encoder, nodes, false, first_generation);
            });

            const auto observed = harness.read(*harness.m_results);
            for (std::size_t index = 0u; index < nodes.size(); ++index) {
                const u32 flat = static_cast<u32>(nodes[index][1u] * 18 + nodes[index][0u]);
                PPR_TEST_ASSERT(observed[index * 13u + 9u] == 1u);
                PPR_TEST_ASSERT(observed[index * 13u + 5u] == dense[flat * 4u]);
                PPR_TEST_ASSERT(observed[index * 13u + 10u] == dense[flat * 4u + 1u]);
                PPR_TEST_ASSERT(observed[index * 13u + 11u] == dense[flat * 4u + 2u]);
                PPR_TEST_ASSERT(observed[index * 13u + 12u] == dense[flat * 4u + 3u]);
            }
        }
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
        _.log(
            std::format(
                "[terrain-wave-check] threads=64 wave_lanes_min={} wave_lanes_max={}", minimum_width, maximum_width));
    };

    // ------------------------------------------------------------------
    // published terrain recovery and continuous point contact
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_rounded_v_endpoint_preserves_certified_contacts_and_resident_rest) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 2055u, 2055u, true, nullptr, 0.0f);
        const std::array<u32, 2u> masks{13u, 14u};
        bool exterior_passed = true;
        u32 certified_starts = 0u;
        for (const auto origin: {std::array<float, 2u>{1023.0f, 7.5f}, std::array<float, 2u>{7.5f, 1023.0f}}) {
            const TerrainView terrain{masks, 2u, 1u, origin[0u], origin[1u]};
            for (const float fraction: {1.0f, std::nextafter(1.0f, 0.0f), 1.0f - 0x1p-17f}) {
                const auto probe = contactProbe(
                    fixture, terrain, {}, 0.0f, {}, 10'000'000, 0.0f, std::array<float, 2u>{fraction, 0.0f}, {}, {},
                    false, false, false, true);
                _.log(
                    std::format(
                        "TERRAIN-V-EXTERIOR origin={},{} fraction_bits={} before={},{} before_certified={} paired={} incoming={},{} after={},{} after_certified={} flags={} field={}",
                        origin[0u], origin[1u], std::bit_cast<u32>(fraction),
                        std::bit_cast<float>(probe.m_contacts[0u]), std::bit_cast<float>(probe.m_contacts[1u]),
                        probe.m_contacts[2u], probe.m_contacts[3u], std::bit_cast<float>(probe.m_contacts[4u]),
                        std::bit_cast<float>(probe.m_contacts[5u]), std::bit_cast<float>(probe.m_contacts[6u]),
                        std::bit_cast<float>(probe.m_contacts[7u]), probe.m_contacts[8u], probe.m_errors[0u],
                        probe.m_errors[3u]));
                exterior_passed = exterior_passed and probe.m_errors[0u] == 0u and probe.m_contacts[8u] == 1u;
                if (probe.m_contacts[2u] == 1u) {
                    ++certified_starts;
                    exterior_passed = exterior_passed and
                                      probe.m_contacts[0u] == probe.m_contacts[6u] and
                                      probe.m_contacts[1u] == probe.m_contacts[7u];
                }
            }
        }

        const TerrainView terrain{masks, 2u, 1u, 1023.0f, 7.5f};
        bool transport_passed = true;
        for (const auto motion:
            {std::array<float, 2u>{0.0f, 0.0f}, std::array<float, 2u>{0.00015f, 0.00035f},
                std::array<float, 2u>{-0.0002f, -0.0002f}, std::array<float, 2u>{0.0002f, -0.0002f}}) {
            const auto probe = contactProbe(
                fixture, terrain, {}, motion[1u], {}, 10'000'000, motion[0u],
                std::array<float, 2u>{1023.99994f, 7.9999f}, {}, {}, false, false, true);
            const float x = std::bit_cast<float>(probe.m_contacts[0u]);
            const float y = std::bit_cast<float>(probe.m_contacts[1u]);
            // The two canonical segments form y=8-|x-1024|. Compare exact
            // published FP32 coordinates in double, without a collision skin.
            const bool outside = std::isfinite(x) and
                                 std::isfinite(y) and
                                 double(y) + std::abs(double(x) - 1024.0) <= 8.0;
            _.log(
                std::format(
                    "TERRAIN-V-TRANSPORT motion={},{} point={},{} carry={},{} first_hit={} full_flags={} final_flags={} field={} outside={}",
                    motion[0u], motion[1u], x, y, std::bit_cast<float>(probe.m_contacts[2u]),
                    std::bit_cast<float>(probe.m_contacts[3u]), probe.m_contacts[4u], probe.m_contacts[11u],
                    probe.m_errors[0u], probe.m_errors[3u], outside));
            // The helper subsequently runs a separate one-contact-budget probe;
            // contacts[11] is the ordinary full transport's prior error result.
            transport_passed = transport_passed and probe.m_contacts[11u] == 0u and outside;
        }

        bool resident_passed = true;
        for (const bool resting: {true, false}) {
            GpuParticle initial = restParticle();
            initial.m_material = static_cast<float>(ESourceMaterial::sand);
            initial.m_position_x = resting ? 1024.0f : 1023.99994f;
            initial.m_position_y = resting ? 8.0f : 7.9999f;
            initial.m_displacement_x = resting ? 0.0f : 0.00015f;
            initial.m_displacement_y = resting ? 0.0f : 0.00035f;
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                {&initial, 1u}, {}, fixture.m_solver.accounting().m_epoch, terrain));
            fixture.m_solver.publishReplacement();
            for (u32 substep = 0u; substep < 8u; ++substep) {
                const u64 epoch = fixture.m_solver.accounting().m_epoch;
                PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
                PPR_TEST_ASSERT(not fixture.m_solver.drain());
                const auto ready = fixture.m_solver.pollTick();
                // A recoverable terrain-position verdict is a value whose outcome is
                // not publishable; report it as the rejected observation instead of
                // falling through to the completed-tick assertion.
                if (not ready or ready->rejectedTerrainPosition()) {
                    const auto failure = fixture.m_solver.failure();
                    _.log(
                        std::format(
                            "TERRAIN-V-RESIDENT resting={} substep={} epoch={} flags={} field={} rejected=true",
                            resting, substep, epoch, failure.m_flags, failure.m_field));
                    PPR_TEST_ASSERT(not fixture.m_solver.discardTick());
                    resident_passed = false;
                    break;
                }

                PPR_TEST_ASSERT(ready and *ready);
                fixture.m_solver.publishTick();
                const auto rows = fixture.snapshot();
                PPR_TEST_ASSERT(rows.size() == 1u);
                const auto &particle = rows[0u];
                const bool outside = double(particle.m_position_y) + std::abs(double(particle.m_position_x) - 1024.0) <=
                                     8.0;
                _.log(
                    std::format(
                        "TERRAIN-V-RESIDENT resting={} substep={} epoch={} point={},{} carry={},{} outside={} F=[{},{},{},{}] D=[{},{},{},{}] log_jp={}",
                        resting, substep, epoch, particle.m_position_x, particle.m_position_y,
                        particle.m_displacement_x, particle.m_displacement_y, outside,
                        particle.m_deformation_gradient.m_c0x, particle.m_deformation_gradient.m_c0y,
                        particle.m_deformation_gradient.m_c1x, particle.m_deformation_gradient.m_c1y,
                        particle.m_deformation_displacement.m_c0x, particle.m_deformation_displacement.m_c0y,
                        particle.m_deformation_displacement.m_c1x, particle.m_deformation_displacement.m_c1y,
                        particle.m_log_jp));
                for (const float field: std::bit_cast<std::array<float, 24u>>(particle)) {
                    PPR_TEST_ASSERT(std::isfinite(field));
                }

                resident_passed = resident_passed and outside;
                PPR_TEST_ASSERT(particle.m_mass == initial.m_mass and particle.m_volume == initial.m_volume);
                if (resting) {
                    PPR_TEST_ASSERT(particle.m_displacement_x == 0.0f and particle.m_displacement_y == 0.0f);
                    PPR_TEST_ASSERT(
                        std::bit_cast<std::array<float, 4u>>(particle.m_deformation_gradient) ==
                        std::bit_cast<std::array<float, 4u>>(initial.m_deformation_gradient));
                    PPR_TEST_ASSERT(
                        std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement) ==
                        std::bit_cast<std::array<float, 4u>>(initial.m_deformation_displacement));
                }
            }
        }

        PPR_TEST_ASSERT(certified_starts > 0u);
        PPR_TEST_ASSERT(exterior_passed and transport_passed and resident_passed);
    };

    PPR_UNIT_TEST (terrain_recovery_sweeps_and_rest_preserve_published_contact) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(true, 64u, 64u, true, nullptr, 0.0f);

        // The contact contract below is the zero-gravity one, so the case
        // initializes that profile directly instead of discarding a default
        // solver and rebuilding it.
        GpuParameters parameters = fixture.m_solver.profile();
        PPR_TEST_ASSERT(parameters.m_gravity_strength == 0.0f);
        PPR_TEST_ASSERT(parameters.m_delta_time == 1.0f / 480.0f);
        PPR_TEST_ASSERT(parameters.m_liquid_relaxation == 1.0f);

        // Eight FP32 relative roundoff units cover the short geometric
        // calculations at these coordinates, without a physical contact skin.
        const auto tolerance = [](const float coordinate) {
            return 8.0f * std::numeric_limits<float>::epsilon() * std::max(1.0f, std::abs(coordinate));
        };

        const auto valid = [&](const GpuParticle &particle, const GpuParticle &initial) {
            for (const float field: std::bit_cast<std::array<float, 24u>>(particle))
                PPR_TEST_ASSERT(std::isfinite(field));

            PPR_TEST_ASSERT(particle.m_mass == initial.m_mass and particle.m_volume == initial.m_volume);
            PPR_TEST_ASSERT(particle.m_material == initial.m_material and particle.m_enabled == initial.m_enabled);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_particle_count == 1u);
            PPR_TEST_ASSERT(fixture.m_solver.failure().m_flags == 0u);
        };

        const auto replace = [&](const GpuParticle &initial, const TerrainView terrain) {
            PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
                {&initial, 1u}, {}, fixture.m_solver.accounting().m_epoch, terrain));
            fixture.m_solver.publishReplacement();
        };

        const auto step = [&](const GpuParticle &initial) {
            const u64 epoch = fixture.m_solver.accounting().m_epoch;

            fixture.finishOneSubstep();
            const auto rows = fixture.publishSnapshot(epoch);
            PPR_TEST_ASSERT(rows.size() == 1u);
            valid(rows[0u], initial);
            return rows[0u];
        };

        const auto outside = [&](const GpuParticle &particle, const TerrainView terrain) {
            const auto contact = compareTerrainSearch(fixture, terrain, {particle.m_position_x, particle.m_position_y});
            PPR_TEST_ASSERT(contact.m_errors[0u] == 0u);
            const float depth = std::bit_cast<float>(contact.m_contacts[1u]);
            const float scale = std::max(std::abs(particle.m_position_x), std::abs(particle.m_position_y));
            PPR_TEST_ASSERT(contact.m_contacts[0u] == 0u or depth <= tolerance(scale));
        };

        const auto sample_masks = [](const auto &solid) {
            std::vector<u32> masks(32u * 32u);
            for (u32 y = 0u; y < 32u; ++y) {
                for (u32 x = 0u; x < 32u; ++x) {
                    masks[y * 32u + x] = (solid(x, y) ? 1u : 0u) |
                                         (solid(x + 1u, y) ? 2u : 0u) |
                                         (solid(x + 1u, y + 1u) ? 4u : 0u) |
                                         (solid(x, y + 1u) ? 8u : 0u);
                }
            }

            return masks;
        };

        const auto floor_masks = rectangleMasks(32u, 32u, 8u);
        const auto ceiling_masks = sample_masks(
            [](const u32 x, const u32 y) { return x > 0u and x < 32u and y >= 24u and y < 32u; });
        const auto wall_masks = sample_masks(
            [](const u32 x, const u32 y) { return x >= 16u and x < 32u and y > 0u and y < 32u; });

        // Reflections and admitted half-cell translations must retain the same
        // same-step recovery and inward-velocity contract at the actual point.
        for (const float shift: {-0.5f, 0.0f, 0.5f}) {
            for (u32 orientation = 0u; orientation < 3u; ++orientation) {
                const auto &masks = orientation == 0u ? floor_masks : orientation == 1u ? ceiling_masks : wall_masks;
                const TerrainView terrain{masks, 32u, 32u, 3.0f + shift, 3.0f + shift};
                const float boundary = (orientation == 0u ? 11.5f : orientation == 1u ? 26.5f : 18.5f) + shift;
                const float normal = orientation == 0u ? 1.0f : -1.0f;
                for (const bool at_boundary: {false, true}) {
                    GpuParticle initial = restParticle();
                    initial.m_position_x = 20.5f + shift;
                    initial.m_position_y = 20.5f + shift;
                    float &position = orientation == 2u ? initial.m_position_x : initial.m_position_y;
                    float &displacement = orientation == 2u ? initial.m_displacement_x : initial.m_displacement_y;
                    position = boundary - (at_boundary ? 0.0f : normal * 0.125f);
                    displacement = at_boundary ? -normal * 0.25f : 0.0f;
                    replace(initial, terrain);

                    const GpuParticle particle = step(initial);
                    const float coordinate = orientation == 2u ? particle.m_position_x : particle.m_position_y;
                    const float carry = orientation == 2u ? particle.m_displacement_x : particle.m_displacement_y;
                    PPR_TEST_ASSERT(normal * (coordinate - boundary) >= -tolerance(boundary));
                    PPR_TEST_ASSERT(normal * carry >= -tolerance(boundary));
                    outside(particle, terrain);

                    if (not at_boundary) {
                        PPR_TEST_ASSERT(std::abs(coordinate - boundary) <= tolerance(boundary));
                        PPR_TEST_ASSERT(particle.m_displacement_x == 0.0f and particle.m_displacement_y == 0.0f);
                        PPR_TEST_ASSERT(
                            std::bit_cast<std::array<float, 4u>>(particle.m_deformation_gradient) ==
                            std::bit_cast<std::array<float, 4u>>(initial.m_deformation_gradient));
                        PPR_TEST_ASSERT(
                            std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement) ==
                            std::bit_cast<std::array<float, 4u>>(initial.m_deformation_displacement));
                        PPR_TEST_ASSERT(particle.m_liquid_density == initial.m_liquid_density);
                    }
                }
            }
        }

        // A point slightly inside the diagonal is recovered geometrically;
        // recovery must not turn into retained velocity or constitutive strain.
        const TerrainView corner_floor{floor_masks, 32u, 32u, 11.5f, 3.0f};
        GpuParticle initial = restParticle();
        initial.m_position_x = 12.1875f;
        initial.m_position_y = 11.125f;
        replace(initial, corner_floor);

        GpuParticle particle = step(initial);
        PPR_TEST_ASSERT(particle.m_position_x - particle.m_position_y <= 1.0f + tolerance(12.5f));
        PPR_TEST_ASSERT(particle.m_displacement_x == 0.0f and particle.m_displacement_y == 0.0f);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<float, 4u>>(particle.m_deformation_gradient) ==
            std::bit_cast<std::array<float, 4u>>(initial.m_deformation_gradient));
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement) ==
            std::bit_cast<std::array<float, 4u>>(initial.m_deformation_displacement));
        PPR_TEST_ASSERT(particle.m_liquid_density == initial.m_liquid_density);
        outside(particle, corner_floor);

        initial.m_position_x = 12.25f;
        initial.m_position_y = 11.25f;
        initial.m_displacement_x = 0.125f;
        initial.m_displacement_y = -0.125f;
        replace(initial, corner_floor);

        particle = step(initial);
        PPR_TEST_ASSERT(particle.m_position_x - particle.m_position_y <= 1.0f + tolerance(12.5f));
        PPR_TEST_ASSERT(particle.m_displacement_x - particle.m_displacement_y <= tolerance(12.5f));
        outside(particle, corner_floor);

        // The wall is only one sample wide: both endpoints of either free
        // trajectory are outside, so endpoint-only projection misses it.
        const auto thin_masks = sample_masks([](const u32 x, const u32 y) { return x == 12u and y > 0u and y < 32u; });
        const TerrainView thin_wall{thin_masks, 32u, 32u, 0.0f, 0.0f};
        for (const float direction: {-1.0f, 1.0f}) {
            initial = restParticle();
            initial.m_position_x = direction > 0.0f ? 9.5f : 14.5f;
            initial.m_position_y = 16.5f;
            initial.m_displacement_x = direction * 6.0f;
            replace(initial, thin_wall);

            particle = step(initial);
            const float boundary = direction > 0.0f ? 11.5f : 12.5f;
            PPR_TEST_ASSERT(direction * (particle.m_position_x - boundary) <= tolerance(boundary));
            PPR_TEST_ASSERT(direction * particle.m_displacement_x <= tolerance(boundary));
            outside(particle, thin_wall);
        }

        // Oblique incidence preserves admissible sliding instead of stopping
        // all transport, including when the first segment ends at a vertex.
        for (const float horizontal: {0.5f, 1.0f}) {
            initial = restParticle();
            initial.m_position_x = 12.0f;
            initial.m_position_y = 11.25f;
            initial.m_displacement_x = horizontal;
            replace(initial, corner_floor);

            particle = step(initial);
            outside(particle, corner_floor);
            PPR_TEST_ASSERT(particle.m_position_x > initial.m_position_x);
            PPR_TEST_ASSERT(particle.m_position_y >= initial.m_position_y - tolerance(12.5f));

            if (particle.m_position_x < 12.5f - tolerance(12.5f)) {
                PPR_TEST_ASSERT(particle.m_displacement_x - particle.m_displacement_y <= tolerance(12.5f));
            } else {
                PPR_TEST_ASSERT(particle.m_position_y >= 11.5f - tolerance(11.5f));
                PPR_TEST_ASSERT(particle.m_displacement_y >= -tolerance(11.5f));
            }
        }

        // A consistent sampled L has mask14 at local cell(16,16). Its notch
        // vertex joins y=19.5 to x+y=38.5; free motion must satisfy both
        // half-planes, while escape and tangency must remain possible.
        const auto concave_masks = sample_masks([](const u32 x, const u32 y) {
            return x > 0u and x < 32u and y > 0u and y < 32u and (x >= 17u or y >= 17u);
        });
        const TerrainView concave{concave_masks, 32u, 32u, 3.0f, 3.0f};
        const std::array<std::array<float, 2u>, 3u> notch_motion{{{0.25f, 0.25f}, {-0.25f, -0.25f}, {-0.25f, 0.0f}}};
        for (const auto &motion: notch_motion) {
            initial = restParticle();
            initial.m_position_x = 19.0f;
            initial.m_position_y = 19.5f;
            initial.m_displacement_x = motion[0u];
            initial.m_displacement_y = motion[1u];
            replace(initial, concave);

            particle = step(initial);
            PPR_TEST_ASSERT(particle.m_position_y <= 19.5f + tolerance(19.5f));
            PPR_TEST_ASSERT(particle.m_position_x + particle.m_position_y <= 38.5f + tolerance(38.5f));
            PPR_TEST_ASSERT(particle.m_displacement_y <= tolerance(19.5f));
            PPR_TEST_ASSERT(particle.m_displacement_x + particle.m_displacement_y <= tolerance(19.5f));
            outside(particle, concave);

            if (motion[0u] < 0.0f) {
                PPR_TEST_ASSERT(particle.m_position_x < initial.m_position_x);
                PPR_TEST_ASSERT(particle.m_displacement_x < 0.0f);
            }
        }

        // The same swept path first reaches a flat floor, then the corner
        // toward a right wall. A reduced budget must reject it, while the
        // production budget completes both geometric contacts successfully.
        const auto funnel_masks = sample_masks([](const u32 x, const u32 y) {
            return x > 0u and x < 32u and y > 0u and y < 32u and (y <= 8u or x >= 24u);
        });
        const TerrainView funnel{funnel_masks, 32u, 32u, 3.0f, 3.0f};
        const auto budget = contactProbe(
            fixture, funnel, {}, -4.0f, {}, 10'000'000, 8.0f, std::array<float, 2u>{20.5f, 13.5f}, {}, {}, false, false,
            true);
        PPR_TEST_ASSERT(budget.m_contacts[4u] == 1u and budget.m_contacts[5u] == 1u);
        PPR_TEST_ASSERT(budget.m_contacts[11u] == 0u);
        PPR_TEST_ASSERT((budget.m_errors[0u] & 256u) != 0u);
        PPR_TEST_ASSERT(budget.m_errors[3u] == 7u and budget.m_errors[4u] == 0u);
        for (std::size_t field = 0u; field < 4u; ++field)
            PPR_TEST_ASSERT(std::isfinite(std::bit_cast<float>(budget.m_contacts[field])));

        PPR_TEST_ASSERT(std::bit_cast<float>(budget.m_contacts[0u]) <= 26.5f + tolerance(26.5f));
        PPR_TEST_ASSERT(std::bit_cast<float>(budget.m_contacts[1u]) >= 11.5f - tolerance(11.5f));
        PPR_TEST_ASSERT(std::bit_cast<float>(budget.m_contacts[6u]) < std::bit_cast<float>(budget.m_contacts[8u]));

        // Consuming one contact still permits the terminal no-hit query to
        // finish the remaining sliding leg with the same one-contact budget.
        const auto sliding = contactProbe(
            fixture, {floor_masks, 32u, 32u, 3.0f, 3.0f}, {}, -4.0f, {}, 10'000'000, 2.0f,
            std::array<float, 2u>{20.5f, 13.5f}, {}, {}, false, false, true);
        PPR_TEST_ASSERT(sliding.m_contacts[4u] == 1u and sliding.m_contacts[5u] == 0u);
        PPR_TEST_ASSERT(sliding.m_errors[0u] == 0u and sliding.m_contacts[11u] == 0u);
        PPR_TEST_ASSERT(std::bit_cast<float>(sliding.m_contacts[0u]) > 21.5f);
        PPR_TEST_ASSERT(std::bit_cast<float>(sliding.m_contacts[1u]) >= 11.5f);

        // Rest recovery is independent of the constitutive material branch.
        // Repeated zero-force contact must not manufacture pressure or strain.
        const TerrainView floor{floor_masks, 32u, 32u, 3.0f, 3.0f};
        for (const auto material:
            {ESourceMaterial::liquid, ESourceMaterial::elastic, ESourceMaterial::sand, ESourceMaterial::visco}) {
            initial = restParticle();
            initial.m_position_x = 20.5f;
            initial.m_position_y = 11.375f;
            initial.m_material = static_cast<float>(material);
            replace(initial, floor);

            for (u32 substep = 0u; substep < 16u; ++substep) {
                particle = step(initial);
                PPR_TEST_ASSERT(particle.m_position_y >= 11.5f - tolerance(11.5f));
                PPR_TEST_ASSERT(particle.m_displacement_x == 0.0f and particle.m_displacement_y == 0.0f);
                PPR_TEST_ASSERT(
                    std::bit_cast<std::array<float, 4u>>(particle.m_deformation_gradient) ==
                    std::bit_cast<std::array<float, 4u>>(initial.m_deformation_gradient));
                PPR_TEST_ASSERT(
                    std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement) ==
                    std::bit_cast<std::array<float, 4u>>(initial.m_deformation_displacement));
                PPR_TEST_ASSERT(particle.m_liquid_density == initial.m_liquid_density);
                outside(particle, floor);
            }

            // Impact exercises the normal constitutive update as well as
            // contact. Observe a bounded sequence rather than asserting rest
            // identity for deformation generated by the physical transport.
            initial.m_position_y = 11.625f;
            initial.m_displacement_y = -0.25f;
            replace(initial, floor);

            for (u32 substep = 0u; substep < 8u; ++substep) {
                particle = step(initial);
                PPR_TEST_ASSERT(particle.m_position_y >= 11.5f - tolerance(11.5f));
                PPR_TEST_ASSERT(particle.m_liquid_density > 0.0f);
                outside(particle, floor);
            }
        }

        // Admission accepts individual canonical masks, but this isolated
        // interior cell deliberately has orphaned contour vertices. Detecting
        // that malformed manifold must reject the working transaction.
        std::vector<u32> orphan_masks(32u * 32u);
        orphan_masks[16u * 32u + 16u] = 1u;
        const TerrainView orphan{orphan_masks, 32u, 32u, 3.0f, 3.0f};
        initial = restParticle();
        initial.m_position_x = 19.5f;
        initial.m_position_y = 19.0f;
        replace(initial, orphan);
        const u64 rejected_epoch = fixture.m_solver.accounting().m_epoch;
        const auto committed = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(committed and committed->m_epoch == rejected_epoch);

        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(rejected_epoch, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());

        const auto rejected = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::result_out_of_range);
        PPR_TEST_ASSERT((fixture.m_solver.failure().m_flags & 256u) != 0u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == rejected_epoch);
        PPR_TEST_ASSERT(committed->m_live_epoch->load() == rejected_epoch);

        PPR_TEST_ASSERT(not fixture.m_solver.discardTick());
        const auto unchanged = fixture.snapshot();
        PPR_TEST_ASSERT(unchanged.size() == 1u);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 24u>>(unchanged[0u]) == std::bit_cast<std::array<u32, 24u>>(initial));

        initial = restParticle();
        initial.m_position_x = 20.5f;
        initial.m_position_y = 11.625f;
        replace(initial, floor);
        particle = step(initial);
        outside(particle, floor);

        const auto rejected_input = [&](const GpuParticle &input, const TerrainView terrain, const u32 flags) {
            replace(input, terrain);
            const u64 epoch = fixture.m_solver.accounting().m_epoch;
            const auto view = fixture.m_solver.committedView();
            PPR_TEST_ASSERT(view and view->m_epoch == epoch);

            PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
            PPR_TEST_ASSERT(not fixture.m_solver.drain());

            const auto failed = fixture.m_solver.pollTick();
            PPR_TEST_ASSERT(not failed and failed.error() == std::errc::result_out_of_range);
            PPR_TEST_ASSERT((fixture.m_solver.failure().m_flags & flags) != 0u);
            PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
            PPR_TEST_ASSERT(view->m_live_epoch->load() == epoch);

            PPR_TEST_ASSERT(not fixture.m_solver.discardTick());
            const auto rows = fixture.snapshot();
            PPR_TEST_ASSERT(rows.size() == 1u);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 24u>>(rows[0u]) == std::bit_cast<std::array<u32, 24u>>(input));
        };

        // Both trajectory endpoints miss this finite solid domain. Entering
        // an incomplete slab must still reject the unpublishable transaction.
        const std::array<u32, 1u> full_cell{15u};
        initial = restParticle();
        initial.m_position_x = 9.5f;
        initial.m_position_y = 11.5f;
        initial.m_displacement_x = 4.0f;
        rejected_input(initial, {full_cell, 1u, 1u, 11.0f, 11.0f}, 16u | 256u);

        // A represented contour endpoint admits an exactly tangent entry and
        // an escaping ray. These remain valid despite touching the domain.
        const std::array<u32, 1u> endpoint_cell{1u};
        for (const float downward: {-1.0f, -0.5f}) {
            const auto endpoint = contactProbe(
                fixture, {endpoint_cell, 1u, 1u, 11.0f, 11.0f}, {}, downward, {}, 10'000'000, 1.0f,
                std::array<float, 2u>{10.5f, 12.0f}, {}, {}, false, false, true);
            PPR_TEST_ASSERT(endpoint.m_errors[0u] == 0u and endpoint.m_contacts[11u] == 0u);
            PPR_TEST_ASSERT(std::bit_cast<float>(endpoint.m_contacts[0u]) == 11.5f);
            PPR_TEST_ASSERT(std::bit_cast<float>(endpoint.m_contacts[1u]) >= 11.0f);
        }

        // A finite admitted gravity can overflow the grid-height product.
        // Contact must reject its nonfinite carry instead of hiding it by
        // projecting the inward component to zero.
        PPR_TEST_ASSERT(not fixture.m_solver.shutdown());
        parameters.m_gravity_strength = std::numeric_limits<float>::max();
        PPR_TEST_ASSERT(not fixture.m_solver.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            parameters));
        initial = restParticle();
        initial.m_position_x = 20.5f;
        initial.m_position_y = 11.5f;
        rejected_input(initial, floor, 8u);

        // At world-scale X, the separately rounded target is not parallel to
        // the diagonal even though the authored transport is. Test the actual
        // published FP32 point using local coordinates, avoiding cancellation
        // in a world-space x+y comparison.
        PPR_TEST_ASSERT(not fixture.m_solver.shutdown());
        parameters.m_grid_width = 4096u;
        parameters.m_grid_height = 512u;
        parameters.m_gravity_strength = 0.0f;
        PPR_TEST_ASSERT(not fixture.m_solver.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            parameters));
        const std::array<u32, 1u> diagonal_cell{1u};
        const TerrainView large_diagonal{diagonal_cell, 1u, 1u, 3175.5f, 430.5f};
        initial = restParticle();
        initial.m_position_x = 3175.75f;
        initial.m_position_y = std::nextafter(430.75f, std::numeric_limits<float>::infinity());
        initial.m_displacement_x = 0.0001f;
        initial.m_displacement_y = -0.0001f;
        replace(initial, large_diagonal);

        particle = step(initial);
        PPR_TEST_ASSERT((particle.m_position_x - 3175.5f) + (particle.m_position_y - 430.5f) >= 0.5f);
        const auto rounded_contact = compareTerrainSearch(
            fixture, large_diagonal, {particle.m_position_x, particle.m_position_y});
        PPR_TEST_ASSERT(rounded_contact.m_errors[0u] == 0u);
        PPR_TEST_ASSERT(
            rounded_contact.m_contacts[0u] == 0u or std::bit_cast<float>(rounded_contact.m_contacts[1u]) == 0.0f);
    };

    // ------------------------------------------------------------------
    // complete terrain boundary search and moving-body contacts
    // ------------------------------------------------------------------

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
        const TerrainView terrain{masks, 8u, 4u, 3.5f, 5.5f};
        for (const float dy: {0.0f, -0.25f, 0.25f}) {
            const GridResult result = gridContact(fixture, terrain, {}, dy);
            PPR_TEST_ASSERT(result.m_errors[0] == 0u);
            PPR_TEST_ASSERT(result.m_node[0] == 0);
            PPR_TEST_ASSERT(result.m_node[1] == (dy > 0.0f ? 2'500'000 : 0));
        }

        // The centre of either disconnected saddle is outside both triangles.
        for (const u32 saddle: {5u, 10u}) {
            const std::array<u32, 1u> mask{saddle};
            const GridResult result = gridContact(fixture, {mask, 1u, 1u, 6.0f, 7.0f}, {}, 0.125f);
            PPR_TEST_ASSERT(result.m_errors[0] == 0u and result.m_node[1] == 1'250'000);
        }

        // An unsupported node must not query a solid mask lacking a boundary.
        // Supported admission of that same incomplete geometry reports flag16.
        const std::array<u32, 1u> solid_mask{15u};
        const TerrainView incomplete{solid_mask, 1u, 1u, 6.0f, 7.0f};
        const GridResult empty = gridContact(fixture, incomplete, {}, 0.0f, {}, 0);
        PPR_TEST_ASSERT(empty.m_errors[0] == 0u and empty.m_node[1] == 0);
        const GridResult missing_boundary = gridContact(fixture, incomplete, {}, 0.0f);
        PPR_TEST_ASSERT((missing_boundary.m_errors[0] & 16u) != 0u);

        // The unique nearest boundary is eight cells above the sample (left
        // 8.5, bottom 9, right 14.5). Its power-of-two depth makes normalization
        // exact for this dyadic geometry; no CPU/GPU division-parity assumption.
        // Recovery reaches the surface in this publication, without storing
        // the positional correction as outward velocity for the next substep.
        const auto deep_masks = rectangleMasks(24u, 24u, 17u);
        GpuParticle particle = restParticle();
        particle.m_position_x = 12.5f;
        particle.m_position_y = 12.5f;
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            {&particle, 1u}, {}, fixture.m_solver.accounting().m_epoch, {deep_masks, 24u, 24u, 3.5f, 3.0f}));
        fixture.m_solver.publishReplacement();
        fixture.finishOneSubstep();
        const GpuParticle candidate = fixture.publishSnapshot(fixture.m_solver.accounting().m_epoch)[0];
        PPR_TEST_ASSERT(candidate.m_position_x == 12.5f);
        PPR_TEST_ASSERT(candidate.m_position_y >= 20.5f);
        PPR_TEST_ASSERT(candidate.m_position_y <= std::nextafter(20.5f, std::numeric_limits<float>::infinity()));
        PPR_TEST_ASSERT(candidate.m_displacement_x == 0.0f and candidate.m_displacement_y == 0.0f);

        // A replacement with the same dimensions moves its boundary to a new
        // tile row. Depth four retains the original power-of-two normalization
        // premise; the old table has no boundary in the new interior tile.
        const auto changed_masks = rectangleMasks(24u, 24u, 13u);
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(
            {&particle, 1u}, {}, fixture.m_solver.accounting().m_epoch, {changed_masks, 24u, 24u, 3.5f, 3.0f}));
        fixture.m_solver.publishReplacement();
        fixture.finishOneSubstep();

        const GpuParticle changed = fixture.publishSnapshot(fixture.m_solver.accounting().m_epoch)[0];
        PPR_TEST_ASSERT(changed.m_position_x == 12.5f);
        PPR_TEST_ASSERT(changed.m_position_y >= 16.5f);
        PPR_TEST_ASSERT(changed.m_position_y <= std::nextafter(16.5f, std::numeric_limits<float>::infinity()));
        PPR_TEST_ASSERT(changed.m_displacement_x == 0.0f and changed.m_displacement_y == 0.0f);
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
        // Semantic rows have 24 FP32 words; device rows add one u32 property index.
        constexpr u64 kRowBytes = sizeof(GpuParticle);
        constexpr u64 kDeviceRowBytes = sizeof(ParticleWrite) + sizeof(ParticleReadonly);
        constexpr u64 kBucketEdge = (kEdge + 5u) / 6u;
        constexpr u64 kBucketCount = kBucketEdge * kBucketEdge;
        constexpr u64 kPageEdge = (kEdge + 7u) / 8u;
        constexpr u64 kPageCount = kPageEdge * kPageEdge;
        constexpr u64 kLookupBytes = 8u * kBucketCount + 16u * kPageCount;
        static_assert(kRowBytes == 96u and kLookupBytes == 7'953'552u);
        static_assert(kDeviceRowBytes == 100u); // 18 * 4 hot + (6 * 4 cold + 4 property index).

        // Independent allocation arithmetic, including every zero-row dummy.
        // No solver accounting subtotal or sparseCapacity result is the oracle.
        // Lookup + contour + dummy terrain + body + shape + 76-byte dummy properties.
        constexpr u64 kFixedBytes = kLookupBytes +
                                    sizeof(PackedContourTables) +
                                    sizeof(u32) +
                                    sizeof(GpuBodyBoundary) +
                                    sizeof(GpuStaticShape) +
                                    sizeof(GpuMaterialProperties);
        constexpr auto storageBytes = [=](const u64 count) {
            const u64 rows = std::max<u64>(1u, count);
            const u64 buckets = std::min(count, kBucketCount);
            const u64 groups = std::min(count, (count + 63u * buckets) / 64u);
            const u64 pages = std::min(6u * buckets, kPageCount);
            return 2u * rows * kDeviceRowBytes +
                   16u * std::max<u64>(1u, buckets) +
                   4u * std::max<u64>(1u, pages) +
                   12u * std::max<u64>(1u, 256u * pages) +
                   16u * std::max<u64>(1u, groups) +
                   4u * rows +
                   16u +
                   48u +
                   24u;
        };

        // Both owners include one hot/cold dummy row and one property row.
        constexpr u64 kEmptyPeak = 2u * (kFixedBytes + storageBytes(0u));
        // B=G=13,044; P=6*B=78,264. Numerical device rows now have stride100.
        constexpr u64 kProjectedPeak = 2u * (kFixedBytes + storageBytes(kAdmitted));
        // Fixed allocations (including properties) cancel from growth.
        constexpr u64 kExpectedGrowth = 4u * (kAdmitted - 1u) * kDeviceRowBytes +
                                        2u * (16u * (kAdmitted - 1u) +
                                                 4u * (6u * kAdmitted - 1u) +
                                                 12u * (256u * 6u * kAdmitted - 1u) +
                                                 16u * (kAdmitted - 1u) +
                                                 4u * (kAdmitted - 1u));
        static_assert(kEmptyPeak == 15'908'400u);
        static_assert(kProjectedPeak == 503'544'792u);
        static_assert(kExpectedGrowth == 487'636'392u and kProjectedPeak - kEmptyPeak == kExpectedGrowth);

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

    // ------------------------------------------------------------------
    // constitutive operand rejection witnesses
    // ------------------------------------------------------------------

    /// One isolated row and the validation field its unusable constitutive
    /// operand must report. 0xffff'ffffu admits the row without a witness.
    struct ConstitutiveWitness {
        std::string_view m_name;
        GpuParticle m_particle;
        u32 m_field{0xffff'ffffu};
    };

    struct ConstitutiveObservation {
        std::string_view m_name;
        u32 m_expected_field{0xffff'ffffu};
        u32 m_field{0xffff'ffffu};
        u32 m_particle{0xffff'ffffu};
        u32 m_flags{};
        u32 m_claimed{};
        u32 m_node{0xffff'ffffu};
        std::error_code m_cause{};
        bool m_published{};
        bool m_finite{};
    };

    /// One complete resident K2 tick per witness row. Every case is observed and
    /// logged before anything is asserted, so one wrong case still reports all of
    /// them; a published case also proves its state stayed finite.
    std::vector<ConstitutiveObservation> observeConstitutiveWitnesses(
        GpuSolver &solver, const std::span<const ConstitutiveWitness> witnesses) {
        std::vector<ConstitutiveObservation> observations;
        observations.reserve(witnesses.size());
        for (const ConstitutiveWitness &witness: witnesses) {
            PPR_TEST_ASSERT(not solver.prepareReplacement({&witness.m_particle, 1u}, {}, solver.accounting().m_epoch));
            solver.publishReplacement();

            const u64 committed_epoch = solver.accounting().m_epoch;
            PPR_TEST_ASSERT(not solver.submitResidentTick(committed_epoch, 1u, 2u));
            PPR_TEST_ASSERT(not solver.drain());

            ConstitutiveObservation observation;
            observation.m_name = witness.m_name;
            observation.m_expected_field = witness.m_field;
            const auto outcome = solver.pollTick();
            if (outcome) {
                observation.m_published = true;
                observation.m_flags = solver.failure().m_flags;
                solver.publishTick();

                PPR_TEST_ASSERT(not solver.beginSnapshot(solver.accounting().m_epoch));
                PPR_TEST_ASSERT(not solver.drain());
                const auto captured = solver.pollSnapshot();
                PPR_TEST_ASSERT(captured and captured->has_value());
                PPR_TEST_ASSERT((**captured).m_particles.size() == 1u);
                observation.m_finite = std::ranges::all_of(
                    std::bit_cast<std::array<float, 24u>>((**captured).m_particles[0u]),
                    [](const float field) { return std::isfinite(field); });
            } else {
                const GpuFailure &failure = solver.failure();
                observation.m_cause = outcome.error();
                observation.m_flags = failure.m_flags;
                observation.m_claimed = failure.m_claimed;
                observation.m_node = failure.m_node;
                observation.m_field = failure.m_field;
                observation.m_particle = failure.m_particle;

                PPR_TEST_ASSERT(not solver.discardTick());

                PPR_TEST_ASSERT(solver.accounting().m_epoch == committed_epoch);
                PPR_TEST_ASSERT(not solver.beginSnapshot(committed_epoch));
                PPR_TEST_ASSERT(not solver.drain());
                const auto captured = solver.pollSnapshot();
                PPR_TEST_ASSERT(captured and captured->has_value());
                PPR_TEST_ASSERT((**captured).m_particles.size() == 1u);
                PPR_TEST_ASSERT(
                    std::memcmp(&(**captured).m_particles[0u], &witness.m_particle, sizeof(GpuParticle)) == 0);
            }

            observations.push_back(observation);
        }

        return observations;
    }

    void logConstitutiveWitnesses(UnitTest::IRun &_, const std::span<const ConstitutiveObservation> observations) {
        for (const ConstitutiveObservation &observation: observations) {
            _.log(
                std::format(
                    "CONSTITUTIVE-WITNESS case={} expected_field={} published={} finite={} cause={} flags={} "
                    "claimed={} node={} field={} particle={}",
                    observation.m_name, observation.m_expected_field, observation.m_published, observation.m_finite,
                    observation.m_cause.value(), observation.m_flags, observation.m_claimed, observation.m_node,
                    observation.m_field, observation.m_particle));
        }
    }

    /// A witness case is one fatal per-particle verdict in the existing validation
    /// slot: the guard's flag 8 carries the witness field and row, and the
    /// downstream fixed-point conversion (flag 2) never produced the failure. The
    /// union may also carry flag 128, a pre-existing independent stencil-key or
    /// budget condition that solver.slang and sparse_grid.slang record for their
    /// own invalidity causes; that bit says nothing about constitutive validity,
    /// so the pair below, not the whole union, is the accurate contract.
    void assertConstitutiveWitnesses(const std::span<const ConstitutiveObservation> observations) {
        for (const ConstitutiveObservation &observation: observations) {
            if (observation.m_expected_field == 0xffff'ffffu) {
                PPR_TEST_ASSERT(observation.m_published and observation.m_finite);
                PPR_TEST_ASSERT(observation.m_flags == 0u);
                continue;
            }

            PPR_TEST_ASSERT(not observation.m_published);
            PPR_TEST_ASSERT(observation.m_cause == std::errc::result_out_of_range);
            PPR_TEST_ASSERT((observation.m_flags & 8u) == 8u);
            PPR_TEST_ASSERT((observation.m_flags & 2u) == 0u);
            PPR_TEST_ASSERT(observation.m_claimed == 1u);
            PPR_TEST_ASSERT(observation.m_node == 0xffff'ffffu);
            PPR_TEST_ASSERT(observation.m_field == observation.m_expected_field);
            PPR_TEST_ASSERT(observation.m_particle == 0u);
        }
    }

    PPR_UNIT_TEST (constitutive_invalid_operands_do_not_enter_unsafe_operations) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        // Zero gravity and no grid-volume feedback isolate the constitutive
        // operands. The profile stays inside the admitted domain; the visco
        // profile-domain rejection is exercised separately below.
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);
        PPR_TEST_ASSERT(fixture.m_solver.profile().m_plasticity == 0.0f);

        const auto source = [](const ESourceMaterial identity) {
            GpuParticle particle = restParticle();
            particle.m_material = static_cast<float>(identity);
            return particle;
        };
        const auto elastic = [&] { return source(ESourceMaterial::elastic); };
        const auto rotationMatrix = [](const float angle) {
            SourceMatrix result{};
            result.m_c0x = std::cos(angle);
            result.m_c0y = std::sin(angle);
            result.m_c1x = -std::sin(angle);
            result.m_c1y = std::cos(angle);
            return result;
        };

        std::vector<ConstitutiveWitness> witnesses;

        // (a) Regression coverage plus the explicit liquid reciprocal guard:
        // `invalidParticleField` returns slot 12 for `liquidDensity <= 0`
        // (types.slang), and both `sparseParticle` (sparse_grid.slang) and
        // `validParticle` (solver.slang) report it through
        // `recordError(8u, 0xffffffffu, field, row)`. The stored 1e-39f is a
        // subnormal, so a device that flushes it would take that positivity path;
        // on a device that preserves it, `deformationOperandsUsable` rejects the
        // non-finite `1/density` with the same field 12. Neither path depends on
        // flush-to-zero.
        {
            GpuParticle particle = source(ESourceMaterial::liquid);
            particle.m_liquid_density = 1.0e-39f;
            witnesses.push_back({"liquid_density_reciprocal", particle, 12u});
        }

        // (b) The stored deformation gradient is the zero matrix, so its
        // determinant reciprocal and inverse components are unusable.
        {
            GpuParticle particle = elastic();
            particle.m_deformation_gradient = {0.0f, 0.0f, 0.0f, 0.0f};
            witnesses.push_back({"stored_zero_gradient", particle, 4u});
        }

        // (c) A singular stored gradient that still carries a finite entry.
        {
            GpuParticle particle = elastic();
            particle.m_deformation_gradient = {0.0f, 0.0f, 0.0f, 1.0f};
            witnesses.push_back({"stored_singular_gradient", particle, 4u});
        }

        // (d) The trial deformation (I + D)F is singular although both stored
        // operands are finite.
        {
            GpuParticle particle = elastic();
            particle.m_deformation_displacement = {-1.0f, 0.0f, 0.0f, 0.0f};
            witnesses.push_back({"trial_singular_deformation", particle, 8u});
        }

        // (e) The determinant is exactly one and its reciprocal is finite, but
        // the source radicands overflow: E = F = 5e19 squares past FLT_MAX while
        // the entries themselves stay finite. Reported as the stored gradient.
        {
            GpuParticle particle = elastic();
            particle.m_deformation_gradient = {1.0e20f, 0.0f, 0.0f, 1.0e-20f};
            witnesses.push_back({"svd_intermediate_overflow", particle, 4u});
        }

        // (f) The determinant -1e-30 and its reciprocal are finite in normal
        // range, while the inverse component a/det = 1e19/-1e-30 overflows. All
        // four entries are normal numbers or exact zero, so no flush-to-zero can
        // turn this into a different (singular) case.
        {
            GpuParticle particle = elastic();
            particle.m_deformation_gradient = {1.0e19f, 1.0e-15f, 1.0e-15f, 0.0f};
            witnesses.push_back({"inverse_component_overflow", particle, 4u});
        }

        // (g) The visco yield exponential is a profile constant, so its domain is
        // rejected at resident-tick admission below rather than per particle.

        // (h) A finite but huge plastic history makes the sand deviatoric norm
        // overflow before the projection divides: traceE = -1e30 and eHat = 5e29
        // per component, so length() has no finite value. The sand guard reports
        // the history operand, field 17.
        {
            GpuParticle particle = source(ESourceMaterial::sand);
            particle.m_log_jp = -1.0e30f;
            witnesses.push_back({"sand_norm_overflow", particle, 17u});
        }

        // Controls: identity, a rotation, moderate compression and a stored
        // gradient whose singular values 0.05 fall below the admitted clamp
        // floor. Upstream limiting is legal and must not be flagged.
        {
            witnesses.push_back({"control_identity", elastic(), 0xffff'ffffu});
        }
        {
            GpuParticle particle = elastic();
            particle.m_deformation_gradient = rotationMatrix(0.5f);
            witnesses.push_back({"control_rotation", particle, 0xffff'ffffu});
        }
        {
            GpuParticle particle = elastic();
            particle.m_deformation_gradient = {0.9f, 0.0f, 0.0f, 0.9f};
            witnesses.push_back({"control_compression", particle, 0xffff'ffffu});
        }
        {
            GpuParticle particle = elastic();
            particle.m_deformation_gradient = {0.05f, 0.0f, 0.0f, 0.05f};
            witnesses.push_back({"control_clamped_singular_values", particle, 0xffff'ffffu});
        }

        // Current legal behaviour, not a guard: equal trial singular values 1.5
        // with zero plastic history give traceE = 2*log(1.5) > 0 and a zero
        // deviatoric norm, so the sand history projection takes the dilation
        // branch and never divides. Whether this state should be admitted is the
        // sand-history question, so the row must publish finite state without a
        // constitutive witness.
        {
            GpuParticle particle = source(ESourceMaterial::sand);
            particle.m_deformation_gradient = {1.5f, 0.0f, 0.0f, 1.5f};
            particle.m_log_jp = 0.0f;
            witnesses.push_back({"control_sand_zero_deviatoric_history", particle, 0xffff'ffffu});
        }

        // Antisymmetric D survives liquid viscosity and has zero trace. Its raw
        // 1024 components exceed 2^31 / 1e7, but mass 1e-8 and the quadratic
        // stencil weights (corner 1/64) keep every momentum contribution tiny.
        // Encoded mass is zero at every node, so the next gather is zero under
        // the source tiny-mass rule and the complete K2 tick must publish.
        {
            GpuParticle particle = source(ESourceMaterial::liquid);
            particle.m_deformation_displacement = {0.0f, 1024.0f, -1024.0f, 0.0f};
            particle.m_mass = 1.0e-8f;
            witnesses.push_back({"control_large_finite_D_small_weighted_mass", particle, 0xffff'ffffu});
        }

        // Every case is one complete resident K2 tick over a single row, so the
        // evidence is the production rejection path, not a helper microgate.
        const auto observations = observeConstitutiveWitnesses(fixture.m_solver, witnesses);
        logConstitutiveWitnesses(_, observations);
        assertConstitutiveWitnesses(observations);

        // The visco yield exponential is derived from profile constants, so an
        // unusable domain is refused at resident-tick admission: exp(1 -
        // plasticity) must stay a positive finite bound, and no particle guard or
        // clamp may repair it.
        GpuParameters refused_profile = fixture.m_solver.profile();
        refused_profile.m_plasticity = -100.0f;
        PPR_TEST_ASSERT(not std::isfinite(std::exp(1.0f - refused_profile.m_plasticity)));

        GpuSolver refused;
        PPR_TEST_ASSERT(not refused.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            refused_profile));
        PPR_DEFER {
            PPR_TEST_ASSERT(not refused.shutdown());
        };

        const GpuParticle admitted = restParticle();
        PPR_TEST_ASSERT(not refused.prepareReplacement({&admitted, 1u}, {}, refused.accounting().m_epoch));
        refused.publishReplacement();
        const u64 refused_epoch = refused.accounting().m_epoch;
        const u64 refused_dispatches = refused.accounting().m_dispatches;
        const auto refused_view = refused.committedView();
        PPR_TEST_ASSERT(refused_view and refused_view->m_epoch == refused_epoch);

        PPR_TEST_ASSERT(refused.submitResidentTick(refused_epoch, 1u, 2u) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not refused.busy());
        PPR_TEST_ASSERT(refused.accounting().m_epoch == refused_epoch);
        PPR_TEST_ASSERT(refused.accounting().m_dispatches == refused_dispatches);
        PPR_TEST_ASSERT(refused.failure().m_flags == 0u);
        PPR_TEST_ASSERT(refused_view->m_live_epoch->load() == refused_epoch);

        PPR_TEST_ASSERT(not refused.beginSnapshot(refused_epoch));
        PPR_TEST_ASSERT(not refused.drain());
        const auto refused_capture = refused.pollSnapshot();
        PPR_TEST_ASSERT(refused_capture and refused_capture->has_value());
        PPR_TEST_ASSERT((**refused_capture).m_particles.size() == 1u);
        PPR_TEST_ASSERT(std::memcmp(&(**refused_capture).m_particles[0u], &admitted, sizeof(GpuParticle)) == 0);
    };

    // ------------------------------------------------------------------
    // derived-operand overflow survives admission, so the state guards catch it
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (constitutive_derived_operand_overflow_rejects_state) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);

        // Admission bounds the derived *domain* (a finite, positive yield
        // surface), not the amplitude of coefficient products or of the state
        // they act on. Plasticity 41 leaves yieldSurface = exp(-40) = 4.25e-18,
        // which is admitted, and a huge but finite liquid relaxation admits a
        // coefficient product whose actual scatter operands cannot be encoded.
        GpuParameters profile = fixture.m_solver.profile();
        profile.m_plasticity = 41.0f;
        profile.m_liquid_relaxation = 1.0e30f;
        const float yield_surface = std::exp(1.0f - profile.m_plasticity);
        // Density 0.5 makes the liquid relaxation alpha 0.5, so the produced
        // D is 1e30 * 0.5. At the left centre node, weight 3/32 and
        // offset -1 produce momentum -3/32 * mass * D, far past 2^31
        // after scaling. D itself is finite and has no encoding requirement.
        const float produced_displacement = profile.m_liquid_relaxation * 0.5f;
        const float scatter_momentum = (3.0f / 32.0f) * restParticle().m_mass * produced_displacement;

        PPR_TEST_ASSERT(std::isfinite(yield_surface) and yield_surface > 0.0f);
        PPR_TEST_ASSERT(std::isfinite(produced_displacement));
        PPR_TEST_ASSERT(scatter_momentum * static_cast<float>(profile.m_fixed_point_multiplier) > 2147483648.0f);

        GpuSolver solver;
        PPR_TEST_ASSERT(not solver.initialize(
            fixture.m_rhi->getDevice(), *fixture.m_queue, *fixture.m_shaders, std::filesystem::current_path(),
            profile));
        PPR_DEFER {
            PPR_TEST_ASSERT(not solver.shutdown());
        };

        std::vector<ConstitutiveWitness> witnesses;

        // Trial singular values 1e4 give J = 1e8, while the admitted yield surface
        // clamps both components to 4.25e-18 and newJ to 1.8e-35, so the quotient
        // J / newJ overflows before sqrt. Log-space rescaling must now accept
        // this finite state rather than rejecting an avoidable intermediate.
        {
            GpuParticle particle = restParticle();
            particle.m_material = static_cast<float>(ESourceMaterial::visco);
            particle.m_deformation_gradient = {1.0e4f, 0.0f, 0.0f, 1.0e4f};
            witnesses.push_back({"visco_plastic_quotient_overflow", particle, 0xffff'ffffu});
        }

        // Control: identity leaves the elastic difference exactly zero, so the
        // same admitted profile publishes finite state without a witness.
        {
            GpuParticle particle = restParticle();
            particle.m_material = static_cast<float>(ESourceMaterial::elastic);
            witnesses.push_back({"control_identity_under_admitted_profile", particle, 0xffff'ffffu});
        }

        const auto observations = observeConstitutiveWitnesses(solver, witnesses);
        logConstitutiveWitnesses(_, observations);
        assertConstitutiveWitnesses(observations);

        // checkedEncode rejects the nonzero off-centre momentum with flag 2
        // and returns zero before budget reservation. One row cannot exhaust
        // the mass budget; neither the finite D nor density warrants flag 8.
        GpuParticle particle = restParticle();
        particle.m_material = static_cast<float>(ESourceMaterial::liquid);
        particle.m_liquid_density = 0.5f;
        const std::array<ConstitutiveWitness, 1u> conversion_case{{
            {"liquid_coefficient_product_overflow", particle, 0u},
        }};
        const auto conversion = observeConstitutiveWitnesses(solver, conversion_case);
        logConstitutiveWitnesses(_, conversion);

        const auto &rejected = conversion.front();
        PPR_TEST_ASSERT(not rejected.m_published);
        PPR_TEST_ASSERT(rejected.m_cause == std::errc::result_out_of_range);
        PPR_TEST_ASSERT((rejected.m_flags & 2u) == 2u);
        PPR_TEST_ASSERT((rejected.m_flags & (4u | 8u)) == 0u);
        PPR_TEST_ASSERT(rejected.m_claimed == 1u and rejected.m_node == 0xffff'ffffu);
        PPR_TEST_ASSERT(rejected.m_field == 0u or rejected.m_field == 1u);
        PPR_TEST_ASSERT(rejected.m_particle == 0u);
    };

    // ------------------------------------------------------------------
    // mixed-batch constitutive rejection is transactional
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (constitutive_rejection_preserves_committed_epoch_and_rows) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false, 32u, 32u, true, nullptr, 0.0f);
        PPR_TEST_ASSERT(fixture.m_rhi->isCoreValidationEnabled());
        PPR_TEST_ASSERT(fixture.m_rhi->isGpuAssistedValidationEnabled());
        auto &device = fixture.m_rhi->getDevice();

        const auto elastic = [] {
            GpuParticle particle = restParticle();
            particle.m_material = static_cast<float>(ESourceMaterial::elastic);
            return particle;
        };

        // Rows 0-2 share the 6-cell bucket tile at (1,1) and rows 3-5 share the
        // neighbouring tile at (2,2), so one submission spans two workgroups.
        // Every row is admissible to sparse discovery (finite fields, positive
        // density, in-grid position), so both workgroups really form with valid
        // and unusable rows together and the rejection happens inside the fused
        // stage instead of before group construction.
        std::vector<GpuParticle> rows;

        GpuParticle identity = elastic();
        identity.m_position_x = 6.5f;
        identity.m_position_y = 6.5f;
        rows.push_back(identity);

        GpuParticle zeroGradient = elastic();
        zeroGradient.m_position_x = 7.5f;
        zeroGradient.m_position_y = 6.5f;
        zeroGradient.m_deformation_gradient = {0.0f, 0.0f, 0.0f, 0.0f};
        rows.push_back(zeroGradient);

        GpuParticle liquid = restParticle();
        liquid.m_position_x = 8.5f;
        liquid.m_position_y = 7.5f;
        rows.push_back(liquid);

        // Normal-range entries: det -1e-30 and 1/det -1e30 are finite while the
        // inverse component a/det = -1e49 overflows, with no subnormal component
        // that flush-to-zero could turn into a different (singular) case.
        GpuParticle inverseOverflow = elastic();
        inverseOverflow.m_position_x = 12.5f;
        inverseOverflow.m_position_y = 12.5f;
        inverseOverflow.m_deformation_gradient = {1.0e19f, 1.0e-15f, 1.0e-15f, 0.0f};
        rows.push_back(inverseOverflow);

        GpuParticle rotation = elastic();
        rotation.m_position_x = 13.5f;
        rotation.m_position_y = 12.5f;
        rotation.m_deformation_gradient = {0.0f, 1.0f, -1.0f, 0.0f};
        rows.push_back(rotation);

        GpuParticle singularTrial = elastic();
        singularTrial.m_position_x = 14.5f;
        singularTrial.m_position_y = 13.5f;
        singularTrial.m_deformation_displacement = {-1.0f, 0.0f, 0.0f, 0.0f};
        rows.push_back(singularTrial);

        const u32 row_count = static_cast<u32>(rows.size());

        // Rows 0, 2 and 4 are admissible; rows 1, 3 and 5 are not, with witnesses
        // 4, 4 and 8. The first-error latch may name any rejected row, never an
        // admissible one.
        const auto witness = [](const u32 row) {
            switch (row) {
                case 1u: return 4u;
                case 3u: return 4u;
                case 5u: return 8u;
                default: return 0xffff'ffffu;
            }
        };

        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(rows, {}, fixture.m_solver.accounting().m_epoch));
        fixture.m_solver.publishReplacement();
        const u64 epoch = fixture.m_solver.accounting().m_epoch;

        const auto published = fixture.m_solver.committedView();
        PPR_TEST_ASSERT(published and published->m_epoch == epoch and published->m_count == row_count);
        const auto before = fixture.snapshot();
        PPR_TEST_ASSERT(before.size() == row_count);
        PPR_TEST_ASSERT(std::memcmp(before.data(), rows.data(), rows.size() * sizeof(GpuParticle)) == 0);

        // One submission, one drain: a hang would leave the fence unfinished.
        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(epoch, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto outcome = fixture.m_solver.pollTick();
        const GpuFailure failure = fixture.m_solver.failure();
        _.log(
            std::format(
                "CONSTITUTIVE-MIXED-TICK rows={} epoch={} completed={} cause={} flags={} claimed={} node={} field={} "
                "particle={} expected_field={}",
                row_count, epoch, static_cast<bool>(outcome), failure.m_cause.value(), failure.m_flags,
                failure.m_claimed, failure.m_node, failure.m_field, failure.m_particle, witness(failure.m_particle)));

        // The mixed batch is a single fatal candidate rejection, and the rejection
        // must carry the witness of the row it names. These rows travel the same
        // tick path as the isolated cases and can co-record the independent
        // stencil-key/budget bit 128, so the contract is the guard's flag 8 latched
        // with no downstream fixed-point conversion (flag 2).
        PPR_TEST_ASSERT(not outcome);
        PPR_TEST_ASSERT(outcome.error() == std::errc::result_out_of_range);
        PPR_TEST_ASSERT((failure.m_flags & 8u) == 8u);
        PPR_TEST_ASSERT((failure.m_flags & 2u) == 0u);
        PPR_TEST_ASSERT(failure.m_claimed == 1u);
        PPR_TEST_ASSERT(failure.m_node == 0xffff'ffffu);
        PPR_TEST_ASSERT(failure.m_particle < row_count);
        PPR_TEST_ASSERT(witness(failure.m_particle) == failure.m_field);

        // The documented settle path discards the candidate without touching the
        // published pair, its revocation value or the committed epoch.
        PPR_TEST_ASSERT(not fixture.m_solver.discardTick());
        PPR_TEST_ASSERT(not fixture.m_solver.busy());
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_particle_count == row_count);
        PPR_TEST_ASSERT(published->m_live_epoch->load() == epoch);

        // The committed split rows are read directly because the retained failure
        // cause still bars the snapshot transaction, and a discard must not have
        // rewritten them.
        std::vector<ParticleWrite> published_hot(row_count);
        std::vector<ParticleReadonly> published_cold(row_count);
        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(
            published->m_hot, 0u, published_hot.size() * sizeof(ParticleWrite), published_hot.data())));
        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(
            published->m_cold, 0u, published_cold.size() * sizeof(ParticleReadonly), published_cold.data())));

        for (u32 row = 0u; row < row_count; ++row) {
            const ParticleWrite expected_hot = packWrite(before[row]);
            const ParticleReadonly expected_cold = packReadonly(before[row]);

            PPR_TEST_ASSERT(std::memcmp(&published_hot[row], &expected_hot, sizeof(ParticleWrite)) == 0);
            PPR_TEST_ASSERT(std::memcmp(&published_cold[row], &expected_cold, sizeof(ParticleReadonly)) == 0);
        }

        // The same committed epoch still admits and publishes a clean batch.
        const std::vector<GpuParticle> admissible{rows[0u], rows[2u], rows[4u]};
        PPR_TEST_ASSERT(not fixture.m_solver.prepareReplacement(admissible, {}, epoch));
        fixture.m_solver.publishReplacement();

        PPR_TEST_ASSERT(not fixture.m_solver.submitResidentTick(fixture.m_solver.accounting().m_epoch, 1u, 2u));
        PPR_TEST_ASSERT(not fixture.m_solver.drain());
        const auto recovered = fixture.m_solver.pollTick();
        PPR_TEST_ASSERT(recovered and *recovered);
        fixture.m_solver.publishTick();
        PPR_TEST_ASSERT(fixture.m_solver.failure().m_flags == 0u);
        PPR_TEST_ASSERT(fixture.m_solver.accounting().m_epoch == epoch + 2u);
    };

    // ------------------------------------------------------------------
    // quadratic transfer stencil identities
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (quadratic_transfer_reproduces_affine_fields_and_moments) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);

        constexpr std::array<u32, 4u> kCells{6u, 511u, 1023u, 4095u};
        constexpr std::array<float, 4u> kPhases{0.0f, 0.125f, 0.5f, 0.875f};
        constexpr u32 kWordsPerQuery = 11u;
        const u32 query_count = static_cast<u32>(kCells.size() * kPhases.size() * kPhases.size());

        std::vector<std::array<float, 2u>> points;
        points.reserve(query_count);
        for (const u32 cell: kCells) {
            for (const float phase_y: kPhases) {
                for (const float phase_x: kPhases) {
                    points.push_back({static_cast<float>(cell) + phase_x, static_cast<float>(cell) + phase_y});
                }
            }
        }

        PPR_TEST_ASSERT(points.size() == query_count);

        shader::SharedModule checks;
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "pbmpm" / "source_checks.slang", "pbmpm_source_checks",
            checks.writeRef()));
        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_TEST_ASSERT(not make_error_code(checks->findEntryPointByName("inspectQuadraticWeights", entry.writeRef())));

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

        const auto make_buffer = [&](const u64 bytes, const u32 stride, const void *const initial) {
            rhi::BufferDesc buffer_desc{};
            buffer_desc.size = bytes;
            buffer_desc.elementSize = stride;
            buffer_desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            buffer_desc.defaultState = rhi::ResourceState::UnorderedAccess;
            rhi::ComPtr<rhi::IBuffer> result;
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(buffer_desc, initial, result.writeRef())));
            return result;
        };

        auto points_buffer = make_buffer(
            points.size() * sizeof(points[0u]), static_cast<u32>(sizeof(points[0u])), points.data());
        auto probe_buffer = make_buffer(
            query_count * kWordsPerQuery * sizeof(float), static_cast<u32>(sizeof(float)), nullptr);

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        rhi::IShaderObject *const root = pass->bindPipeline(pipeline);
        PPR_TEST_ASSERT(root != nullptr);
        rhi::ShaderCursor cursor{root};
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_quadratic_points"].setBinding(
                rhi::Binding(points_buffer, {.offset = 0u, .size = points_buffer->getDesc().size}))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_probe"].setBinding(
                rhi::Binding(probe_buffer, {.offset = 0u, .size = probe_buffer->getDesc().size}))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_quadratic_count"].setData(&query_count, sizeof(query_count))));
        pass->dispatchCompute(1u, 1u, 1u);
        pass->end();

        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
        bool may_be_inflight = true;
        PPR_DEFER {
            if (may_be_inflight)
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        };

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        may_be_inflight = false;

        std::vector<float> words(query_count * kWordsPerQuery);
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(probe_buffer, 0u, words.size() * sizeof(float), words.data())));

        // A non-finite probe word would silently keep every accumulator below at
        // its previous value, so the raw stencil output is checked first.
        PPR_TEST_ASSERT(std::ranges::all_of(words, [](const float value) { return std::isfinite(value); }));

        // Test-local affine field and analytic moment reference. The weights are
        // the production stencil; every identity below is evaluated in double.
        constexpr double kMomentTolerance = 2.0e-6;
        constexpr double kAffineTolerance = 2.0e-5;
        const auto affine = [](const double x, const double y) {
            const std::array<double, 2u> value{3.25 * x - 1.5 * y + 0.75, -0.5 * x + 2.0 * y - 1.25};
            return value;
        };

        double worst_unity = 0.0, worst_first_moment = 0.0, worst_second_moment = 0.0, worst_affine = 0.0;
        for (u32 query = 0u; query < query_count; ++query) {
            const float *const row = &words[query * kWordsPerQuery];
            const std::array<double, 3u> weight_x{
                static_cast<double>(row[2u]), static_cast<double>(row[3u]), static_cast<double>(row[4u])};
            const std::array<double, 3u> weight_y{
                static_cast<double>(row[5u]), static_cast<double>(row[6u]), static_cast<double>(row[7u])};
            const double x = static_cast<double>(points[query][0u]);
            const double y = static_cast<double>(points[query][1u]);

            double unity = 0.0, first_x = 0.0, first_y = 0.0;
            double second_xx = 0.0, second_xy = 0.0, second_yy = 0.0;
            std::array<double, 2u> transferred{};
            for (u32 axis_y = 0u; axis_y < 3u; ++axis_y) {
                // Grid node centres are index+0.5 in the solver's cell coordinates.
                const double node_y = static_cast<double>(row[1u]) + static_cast<double>(axis_y) + 0.5;
                for (u32 axis_x = 0u; axis_x < 3u; ++axis_x) {
                    const double node_x = static_cast<double>(row[0u]) + static_cast<double>(axis_x) + 0.5;
                    const double weight = weight_x[axis_x] * weight_y[axis_y];
                    const auto value = affine(node_x, node_y);
                    const double offset_x = node_x - x;
                    const double offset_y = node_y - y;

                    unity += weight;
                    first_x += weight * offset_x;
                    first_y += weight * offset_y;
                    second_xx += weight * offset_x * offset_x;
                    second_xy += weight * offset_x * offset_y;
                    second_yy += weight * offset_y * offset_y;
                    transferred[0u] += weight * value[0u];
                    transferred[1u] += weight * value[1u];
                }
            }

            const auto expected = affine(x, y);
            worst_unity = std::max(worst_unity, std::abs(unity - 1.0));
            worst_first_moment = std::max({worst_first_moment, std::abs(first_x), std::abs(first_y)});
            worst_second_moment = std::max(
                {worst_second_moment, std::abs(second_xx - 0.25), std::abs(second_xy), std::abs(second_yy - 0.25)});
            worst_affine = std::max(
                worst_affine, std::max(
                                  std::abs(transferred[0u] - expected[0u]) / std::max(1.0, std::abs(expected[0u])),
                                  std::abs(transferred[1u] - expected[1u]) / std::max(1.0, std::abs(expected[1u]))));
        }

        _.log(
            std::format(
                "PBMPM-QUADRATIC-TRANSFER cells={} phases={} queries={} worst_unity={:.9g} "
                "worst_first_moment={:.9g} worst_second_moment={:.9g} worst_affine_relative={:.9g}",
                kCells.size(), kPhases.size(), query_count, worst_unity, worst_first_moment, worst_second_moment,
                worst_affine));

        PPR_TEST_ASSERT(worst_unity <= kMomentTolerance);
        PPR_TEST_ASSERT(worst_first_moment <= kMomentTolerance);
        PPR_TEST_ASSERT(worst_second_moment <= kMomentTolerance);
        PPR_TEST_ASSERT(worst_affine <= kAffineTolerance);
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
                detail::MatterGpu::sand_density_and_terrain_contact_instrumented_gpu_cost_diagnostic,
                detail::MatterGpu::deterministic_1024_sparse_dense_scene,
                detail::MatterGpu::isolated_liquid_density_error_decays_without_affine_growth,
                detail::MatterGpu::initialized_rest_gravity_and_publication,
                detail::MatterGpu::sand_reference_occupancy_compression_and_floor_controls_diagnostic,
                detail::MatterGpu::sand_objective_volume_relaxes_compression_without_attraction_and_preserves_restart,
                detail::MatterGpu::shared_grid_coupling_precedes_support_contact_diagnostic,
                detail::MatterGpu::mean_occupancy_gate_blind_spot_census_diagnostic,
                detail::MatterGpu::sand_terrain_friction_is_dissipative_and_preserves_body_and_guardian_order,
                detail::MatterGpu::evolved_liquid_pool_has_no_interior_render_holes,
                detail::MatterGpu::all_materials_preserve_inventory_and_publish_finite_state_at_k2,
                detail::MatterGpu::substance_rows_preserve_defaults_and_reject_invalid_replacements,
                detail::MatterGpu::calibrated_caps_hardening_and_two_regime_edges,
                detail::MatterGpu::calibrated_admission_and_clay_sand_divergence,
                detail::MatterGpu::calibrated_loaded_clay_and_snow_diagnostic,
                detail::MatterGpu::calibrated_loaded_clay_and_snow_complete_with_finite_state,
                detail::MatterGpu::dense_water_magma_snow_resident_ticks_remain_finite,
                detail::MatterGpu::sustained_snow_load_converges_without_inventory_loss,
                detail::MatterGpu::elastic_one_percent_shear_release_diagnostic,
                detail::MatterGpu::same_family_substances_differ_and_unused_rows_do_not_dispatch,
                detail::MatterGpu::device_property_guard_reports_out_of_range_and_family_mismatch,
                detail::MatterGpu::supported_iteration_counts_validate_before_work,
                detail::MatterGpu::iteration_roles_integrate_once_per_substep,
                detail::MatterGpu::sand_history_phase_a_characterization,
                detail::MatterGpu::odd_even_schedule_preserves_sparse_sources,
                detail::MatterGpu::iteration_count_gpu_cost_diagnostic,
                detail::MatterGpu::iteration_count_correctness_diagnostic,
                detail::MatterGpu::resident_precision_and_quantization_diagnostic,
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
                detail::MatterGpu::terrain_rounded_v_endpoint_preserves_certified_contacts_and_resident_rest,
                detail::MatterGpu::terrain_recovery_sweeps_and_rest_preserve_published_contact,
                detail::MatterGpu::transport_exit_repair_is_bounded_and_a_no_op_when_certified,
                detail::MatterGpu::terrain_position_rejection_is_recoverable_and_other_verdicts_stay_fatal,
                detail::MatterGpu::terrain_endpoint_response_and_complete_boundary_search,
                detail::MatterGpu::wave_boundary_search_matches_serial_for_full_mixed_groups,
                detail::MatterGpu::moving_body_static_limit_translation_rotation_and_one_way_contact,
                detail::MatterGpu::body_admission_rejection_and_same_epoch_contact_publication,
                detail::MatterGpu::predicted_body_slices_select_distinct_substep_rows,
                detail::MatterGpu::device_peak_footprint_for_dual_grid_ownership,
                detail::MatterGpu::constitutive_invalid_operands_do_not_enter_unsafe_operations,
                detail::MatterGpu::constitutive_derived_operand_overflow_rejects_state,
                detail::MatterGpu::constitutive_rejection_preserves_committed_epoch_and_rows,
                detail::MatterGpu::quadratic_transfer_reproduces_affine_fields_and_moments,
            });
        };
        return group;
    }
}
