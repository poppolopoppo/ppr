module;
#include "pP/UnitTest.h"
#include <cstring>
#include <slang.h>

module engine.tests.app;

import engine.core;
import engine.app;
import engine.image;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // world light field plan fixtures
    // ------------------------------------------------------------------

    namespace WorldLightFieldTests {
        // Independent re-derivation of the plan's sample/state arithmetic, kept
        // apart from the production implementation so a drift in the level
        // offsets, accumulation or byte formulas fails this pin.
        struct LayoutTotals {
            u64 m_radiance_samples{};
            u64 m_total_probes{};
            u64 m_state_bytes{};
        };

        LayoutTotals expectedLayout_(const u32 width, const u32 height, const u32 level_count) {
            LayoutTotals totals{};
            for (u32 level = 0u; level < level_count; ++level) {
                const u32 spacing = 2u << level;
                const u32 directions = 4u << (2u * level);
                const u64 probes_x = (u64{width} + spacing - 1u) / spacing;
                const u64 probes_y = (u64{height} + spacing - 1u) / spacing;
                totals.m_total_probes += probes_x * probes_y;
                totals.m_radiance_samples += probes_x * probes_y * u64{directions};
            }
            totals.m_state_bytes = totals.m_radiance_samples * sizeof(u8) + totals.m_total_probes * sizeof(u32);
            return totals;
        }

        PPR_UNIT_TEST (plan_1024_and_2048_use_spacing_two_and_equal_level_samples) {
            for (const u32 edge: {1024u, 2048u}) {
                const auto plan = lighting::planWorldLight({.m_width = edge, .m_height = edge, .m_cell_metres = 0.5f});
                PPR_TEST_ASSERT(plan.has_value());
                PPR_TEST_ASSERT(plan->m_level_count == 6u);
                PPR_TEST_ASSERT(plan->m_levels[0].m_spacing_cells == 2u);
                PPR_TEST_ASSERT(plan->m_levels[0].m_directions == 4u);
                PPR_TEST_ASSERT(plan->m_levels[1].m_directions == 16u);
                PPR_TEST_ASSERT(plan->m_levels[2].m_directions == 64u);

                // Intervals start at zero, chain end-to-end and reach the world
                // diagonal bound; narrowed floats stay finite.
                const float far_bound = 2.0f * float(edge + edge) * 0.5f;
                PPR_TEST_ASSERT(plan->m_levels[0].m_interval_near_m == 0.0f);
                for (u32 index = 0; index < plan->m_level_count; ++index) {
                    const auto &level = plan->m_levels[index];
                    PPR_TEST_ASSERT(level.m_sample_count == plan->m_levels[0].m_sample_count);
                    PPR_TEST_ASSERT(std::isfinite(level.m_interval_near_m));
                    PPR_TEST_ASSERT(std::isfinite(level.m_interval_far_m));
                    if (index > 0u) {
                        PPR_TEST_ASSERT(level.m_interval_near_m == plan->m_levels[index - 1u].m_interval_far_m);
                    }
                }
                PPR_TEST_ASSERT(plan->m_levels[plan->m_level_count - 1u].m_interval_far_m >= far_bound);

                // Radiance/state bytes follow the independently computed totals.
                const LayoutTotals totals = expectedLayout_(edge, edge, plan->m_level_count);
                PPR_TEST_ASSERT(plan->m_radiance_samples == totals.m_radiance_samples);
                PPR_TEST_ASSERT(plan->m_radiance_bytes == totals.m_radiance_samples * 8u);
                PPR_TEST_ASSERT(plan->m_state_bytes == totals.m_state_bytes);
            }
        };

        PPR_UNIT_TEST (plan_rejects_zero_dimensions_nonfinite_cell_size_and_excess_levels) {
            const auto reject = [](const lighting::WorldLightSettings &settings) {
                const auto plan = lighting::planWorldLight(settings);
                PPR_TEST_ASSERT(not plan and plan.error() == std::errc::invalid_argument);
            };

            reject({.m_width = 0u, .m_height = 1024u, .m_cell_metres = 0.5f});
            reject({.m_width = 1024u, .m_height = 0u, .m_cell_metres = 0.5f});

            reject({.m_width = 1024u, .m_height = 1024u, .m_cell_metres = 0.0f});
            reject({.m_width = 1024u, .m_height = 1024u, .m_cell_metres = std::numeric_limits<float>::infinity()});
            reject({.m_width = 1024u, .m_height = 1024u, .m_cell_metres = std::numeric_limits<float>::quiet_NaN()});

            reject({.m_width = 1024u, .m_height = 1024u, .m_cell_metres = 0.5f, .m_level_count = 9u});
            reject({.m_width = 1024u, .m_height = 1024u, .m_cell_metres = 0.5f, .m_level_count = 255u});
        };

        // ------------------------------------------------------------------
        // dense world source fixtures
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (source_applies_dirty_chunks_and_preserves_other_cells) {
            const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            const lighting::WorldLightMaterial materials[] = {{.m_kind = 1u}};
            auto source = lighting::WorldLightSource::create(*plan, materials);
            PPR_TEST_ASSERT(source.has_value());

            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source->applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 2u}));
            PPR_TEST_ASSERT(source->cellId(0u, 0u) == 1u);
            PPR_TEST_ASSERT(source->cellId(129u, 0u) == 0u);
            PPR_TEST_ASSERT(source->revision() == 2u);
            PPR_TEST_ASSERT(source->dirtyRect().m_min_x == 0 and source->dirtyRect().m_max_x == 128);
        };

        PPR_UNIT_TEST (source_empty_update_keeps_revision_and_dirty_rect) {
            const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            auto source = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(source.has_value());

            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source->applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 5u}));
            PPR_TEST_ASSERT(source->revision() == 5u);

            const lighting::WorldLightDirtyRect dirty = source->dirtyRect();
            PPR_TEST_ASSERT(not dirty.empty());

            PPR_TEST_ASSERT(not source->applyUpdate({}));
            PPR_TEST_ASSERT(source->revision() == 5u);
            PPR_TEST_ASSERT(source->dirtyRect().m_min_x == dirty.m_min_x);
            PPR_TEST_ASSERT(source->dirtyRect().m_min_y == dirty.m_min_y);
            PPR_TEST_ASSERT(source->dirtyRect().m_max_x == dirty.m_max_x);
            PPR_TEST_ASSERT(source->dirtyRect().m_max_y == dirty.m_max_y);
        };

        PPR_UNIT_TEST (source_rejects_out_of_range_chunk) {
            const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            auto source = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(source.has_value());

            std::vector<u16> chunk(128u * 128u, 0u);
            // 256x256 is a 2x2 chunk grid, so ids 0..3 are valid; id 4 is not.
            const lighting::WorldLightChunkUpdate update{.m_chunk_id = 4u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(
                source->applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 2u}) == std::errc::result_out_of_range);
            PPR_TEST_ASSERT(source->revision() == 0u);

            std::vector<u16> short_chunk(5u, 0u);
            const lighting::WorldLightChunkUpdate undersized{.m_chunk_id = 0u, .m_cell_ids = short_chunk};
            PPR_TEST_ASSERT(
                source->applyUpdate({.m_chunks = {&undersized, 1u}, .m_revision = 2u}) == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (source_applies_overlay_and_bumps_revision) {
            const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            auto source = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(source.has_value());

            const lighting::WorldLightOverlayCell overlay{.m_cell = 3u, .m_coverage = 1.0f};
            PPR_TEST_ASSERT(not source->applyUpdate({.m_overlay = {&overlay, 1u}, .m_revision = 3u}));
            PPR_TEST_ASSERT(source->revision() == 3u);

            // A cell past the world is rejected without mutating the source.
            const lighting::WorldLightOverlayCell outside{.m_cell = 256u * 256u, .m_coverage = 1.0f};
            PPR_TEST_ASSERT(
                source->applyUpdate({.m_overlay = {&outside, 1u}, .m_revision = 4u}) == std::errc::result_out_of_range);
            PPR_TEST_ASSERT(source->revision() == 3u);
        };

        PPR_UNIT_TEST (source_derives_occupancy_word_for_traversal) {
            const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            auto source = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(source.has_value());
            PPR_TEST_ASSERT(source->occupancyWord(0u, 0u) == 0u);

            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u; // cell (0,0) -> block (0,0) -> bit 0
            chunk[8u] = 1u; // cell (8,0) -> block (1,0) -> bit 1
            const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source->applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 1u}));

            const u32 word = source->occupancyWord(0u, 0u);
            PPR_TEST_ASSERT((word & 1u) == 1u);
            PPR_TEST_ASSERT((word & 2u) == 2u);
        };

        PPR_UNIT_TEST (source_overlay_occupies_and_clears_occupancy) {
            const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            auto source = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(source.has_value());

            // A non-overlay vacuum cell stays clear.
            PPR_TEST_ASSERT((source->occupancyWord(0u, 0u) & 1u) == 0u);

            // An overlay-only cell (id 0) with coverage sets its block's occupancy bit.
            const lighting::WorldLightOverlayCell covered{.m_cell = 0u, .m_coverage = 1.0f};
            PPR_TEST_ASSERT(not source->applyUpdate({.m_overlay = {&covered, 1u}, .m_revision = 1u}));
            PPR_TEST_ASSERT((source->occupancyWord(0u, 0u) & 1u) == 1u);
            PPR_TEST_ASSERT(source->overlayCell(0u, 0u).m_coverage == 1.0f);

            // A zeroed overlay row clears the overlay contribution.
            const lighting::WorldLightOverlayCell cleared{.m_cell = 0u};
            PPR_TEST_ASSERT(not source->applyUpdate({.m_overlay = {&cleared, 1u}, .m_revision = 2u}));
            PPR_TEST_ASSERT((source->occupancyWord(0u, 0u) & 1u) == 0u);
            PPR_TEST_ASSERT(source->overlayCell(0u, 0u).m_coverage == 0.0f);

            // Surface emission alone (coverage 0) also occupies.
            const lighting::WorldLightOverlayCell emissive{.m_cell = 0u, .m_emission = {5.0f, 0.0f, 0.0f}};
            PPR_TEST_ASSERT(not source->applyUpdate({.m_overlay = {&emissive, 1u}, .m_revision = 3u}));
            PPR_TEST_ASSERT((source->occupancyWord(0u, 0u) & 1u) == 1u);
        };
    }

    // ------------------------------------------------------------------
    // dense world light source GPU mirror fixtures
    // ------------------------------------------------------------------

    namespace WorldLightGpu {
        // Matches the device-side GpuOverlayRow: per-cell coverage plus per-unit
        // sigma/emission, 28 bytes.
        struct OverlayRow {
            float coverage{};
            float sigma[3]{};
            float emission[3]{};
        };
        static_assert(sizeof(OverlayRow) == 28u and alignof(OverlayRow) == 4u);

        struct Fixture {
            safe_ptr<IShaderService> m_shaders{IShaderService::get()};
            safe_ptr<IRhiService> m_rhi{IRhiService::get()};
            rhi::ComPtr<rhi::ICommandQueue> m_queue;
            FrameFence m_fence;
            lighting::WorldLightSourceGpu m_source;

            void initialize(const u32 width, const u32 height) {
                PPR_TEST_ASSERT(not m_shaders->initialize());
                const auto setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
                const bool validation = setting and *setting == "1";
                m_rhi->setCoreValidation(true);
                m_rhi->setGpuAssistedValidation(validation);
#ifdef _WIN32
                constexpr auto backend = rhi::DeviceType::D3D12;
#else
                constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
                PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
                auto &device = m_rhi->getDevice();
                PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));
                PPR_TEST_ASSERT(not m_fence.initialize(device));

                const auto plan = lighting::planWorldLight(
                    {.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
                PPR_TEST_ASSERT(plan.has_value());
                const lighting::WorldLightMaterial materials[] = {
                    {.m_kind = 1u}, {.m_kind = 2u, .m_sigma = {5.0f, 0.0f, 0.0f}, .m_emission = {0.0f, 0.0f, 0.0f}}};
                PPR_TEST_ASSERT(
                    not m_source.initialize(*m_rhi, *m_shaders, *plan, materials, std::filesystem::current_path()));
            }

            void submit(rhi::ICommandEncoder &encoder) {
                rhi::ComPtr<rhi::ICommandBuffer> commands;
                PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));

                rhi::ICommandBuffer *pointer = commands.get();
                rhi::IFence *fence = m_fence.fence();
                const auto value = m_fence.nextValue();
                const u64 raw = value;
                rhi::SubmitDesc description{};
                description.commandBuffers = &pointer;
                description.commandBufferCount = 1u;
                description.signalFences = &fence;
                description.signalFenceValues = &raw;
                description.signalFenceCount = 1u;
                PPR_TEST_ASSERT(not make_error_code(m_queue->submit(description)));
                m_fence.commitSubmitted(value);
                PPR_TEST_ASSERT(not m_fence.waitFor(value));
            }

            template<typename T>
            std::vector<T> read(rhi::IBuffer *const source) {
                rhi::BufferDesc description{};
                description.size = source->getDesc().size;
                description.elementSize = 0u;
                description.memoryType = rhi::MemoryType::ReadBack;
                description.usage = rhi::BufferUsage::CopyDestination;
                description.defaultState = rhi::ResourceState::CopyDestination;
                description.label = "WorldLightGpu readback";
                rhi::ComPtr<rhi::IBuffer> copy;
                PPR_TEST_ASSERT(
                    not make_error_code(m_rhi->getDevice().createBuffer(description, nullptr, copy.writeRef())));

                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                encoder->setBufferState(source, rhi::ResourceState::CopySource);
                encoder->copyBuffer(copy, 0u, source, 0u, source->getDesc().size);
                encoder->setBufferState(source, rhi::ResourceState::ShaderResource);
                submit(*encoder);

                std::vector<T> result(source->getDesc().size / sizeof(T));
                void *mapped{};
                PPR_TEST_ASSERT(
                    not make_error_code(m_rhi->getDevice().mapBuffer(copy, rhi::CpuAccessMode::Read, &mapped)));
                std::memcpy(result.data(), mapped, result.size() * sizeof(T));
                PPR_TEST_ASSERT(not make_error_code(m_rhi->getDevice().unmapBuffer(copy)));
                return result;
            }

            void shutdown() {
                if (m_queue and make_error_code(m_queue->waitOnHost()))
                    hal::fatalExit("world-light-test-quiescence-unknown");

                PPR_TEST_ASSERT(not m_source.shutdown());
                PPR_TEST_ASSERT(not m_fence.shutdown());
                m_queue.setNull();
                PPR_TEST_ASSERT(not m_rhi->shutdown());
                PPR_TEST_ASSERT(not m_shaders->shutdown());
            }
        };

        PPR_UNIT_TEST (gpu_source_matter_overlay) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;

            // CPU oracle for the exact ids/overlay/occupancy semantics.
            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            auto oracle = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(oracle.has_value());

            // Upload chunk 0 with id 1 at cell (0,0).
            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate chunk_update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_chunks = {&chunk_update, 1u}, .m_revision = 2u}));
            PPR_TEST_ASSERT(not oracle->applyUpdate({.m_chunks = {&chunk_update, 1u}, .m_revision = 2u}));

            // One opaque particle (palette material 0, kind 1) at cell (4,4), and
            // one participating particle (palette material 1, kind 2) at cell (5,5).
            const lighting::WorldLightMatterParticle particle{
                .m_cell = 4u * width + 4u, .m_fill = 1.0f, .m_material = 0u};
            const lighting::WorldLightMatterParticle emitter{
                .m_cell = 5u * width + 5u, .m_fill = 1.0f, .m_material = 1u};
            const lighting::WorldLightMatterParticle particles[] = {particle, emitter};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_particles = particles, .m_revision = 3u}));
            const lighting::WorldLightOverlayCell oracle_cells[] = {
                {.m_cell = 4u * width + 4u, .m_coverage = 1.0f},
                {.m_cell = 5u * width + 5u, .m_sigma = {5.0f, 0.0f, 0.0f}}};
            PPR_TEST_ASSERT(not oracle->applyUpdate({.m_overlay = oracle_cells, .m_revision = 3u}));

            {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not source.encode(*encoder));
                fixture.submit(*encoder);
            }

            // Cell ids: id 1 at (0,0); untouched cells (far from the chunk) stay vacuum.
            const auto ids = fixture.read<u32>(source.cellIds());
            PPR_TEST_ASSERT(ids.size() == u64{width} * height);
            PPR_TEST_ASSERT(ids[0] == 1u);
            PPR_TEST_ASSERT(ids[1] == 0u);
            PPR_TEST_ASSERT(ids[200u * width + 200u] == 0u);

            // Overlay: coverage 1 at (4,4), sigma 5 at (5,5); untouched cells empty.
            const auto overlay = fixture.read<OverlayRow>(source.overlay());
            PPR_TEST_ASSERT(overlay[4u * width + 4u].coverage == 1.0f);
            PPR_TEST_ASSERT(overlay[5u * width + 5u].sigma[0] == 5.0f);
            PPR_TEST_ASSERT(overlay[0].coverage == 0.0f);
            PPR_TEST_ASSERT(overlay[200u * width + 200u].coverage == 0.0f);

            // Occupancy: both (0,0) and (4,4) live in block (0,0); match the CPU oracle.
            const auto occupancy = fixture.read<u32>(source.occupancy());
            PPR_TEST_ASSERT(occupancy.size() == 32u);
            PPR_TEST_ASSERT((occupancy[0] & 1u) == 1u);
            for (u32 block = 0u; block < occupancy.size(); ++block) {
                PPR_TEST_ASSERT(occupancy[block] == oracle->occupancyWord(block, 0u));
            }

            PPR_TEST_ASSERT(source.revision() == 3u);
            PPR_TEST_ASSERT(source.dirtyRect().m_min_x == 0 and source.dirtyRect().m_min_y == 0);
            PPR_TEST_ASSERT(source.dirtyRect().m_max_x == 128 and source.dirtyRect().m_max_y == 128);

            // A failed update (out-of-range chunk) is rejected without mutation.
            const lighting::WorldLightChunkUpdate bad_chunk{.m_chunk_id = 4u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(
                source.applyUpdate({.m_chunks = {&bad_chunk, 1u}, .m_revision = 9u}) == std::errc::result_out_of_range);
            PPR_TEST_ASSERT(source.revision() == 3u);
            PPR_TEST_ASSERT(source.dirtyRect().m_max_x == 128);

            // An empty update preserves revision and the previous GPU bytes.
            PPR_TEST_ASSERT(not source.applyUpdate({}));
            PPR_TEST_ASSERT(source.revision() == 3u);
            PPR_TEST_ASSERT(source.dirtyRect().m_max_x == 128);

            const auto occupancy_after = fixture.read<u32>(source.occupancy());
            PPR_TEST_ASSERT(occupancy_after == occupancy);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (gpu_source_over_capacity_update_preserves_state) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;

            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate chunk_update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_chunks = {&chunk_update, 1u}, .m_revision = 2u}));

            const lighting::WorldLightDirtyRect dirty = source.dirtyRect();

            // More particles than cells is rejected before any mutation.
            std::vector<lighting::WorldLightMatterParticle> particles(u64{width} * height + 1u);
            PPR_TEST_ASSERT(
                source.applyUpdate({.m_particles = particles, .m_revision = 9u}) == std::errc::value_too_large);

            PPR_TEST_ASSERT(source.revision() == 2u);
            PPR_TEST_ASSERT(source.dirtyRect().m_min_x == dirty.m_min_x);
            PPR_TEST_ASSERT(source.dirtyRect().m_min_y == dirty.m_min_y);
            PPR_TEST_ASSERT(source.dirtyRect().m_max_x == dirty.m_max_x);
            PPR_TEST_ASSERT(source.dirtyRect().m_max_y == dirty.m_max_y);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (gpu_source_second_update_replaces_pending_particles) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;

            // Two updates touch the same cell before one encode: the committed
            // particle set of the second replaces the first, so the cell holds
            // fill 0.5, not an accumulated 1.5.
            const lighting::WorldLightMatterParticle first{.m_cell = 4u * width + 4u, .m_fill = 1.0f, .m_material = 0u};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_particles = {&first, 1u}, .m_revision = 2u}));

            const lighting::WorldLightMatterParticle second{
                .m_cell = 4u * width + 4u, .m_fill = 0.5f, .m_material = 0u};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_particles = {&second, 1u}, .m_revision = 3u}));

            {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not source.encode(*encoder));
                fixture.submit(*encoder);
            }

            const auto overlay = fixture.read<OverlayRow>(source.overlay());
            PPR_TEST_ASSERT(overlay[4u * width + 4u].coverage == 0.5f);
            PPR_TEST_ASSERT(source.revision() == 3u);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (gpu_source_multiword_occupancy) {
            constexpr u32 width = 512u;
            constexpr u32 height = 64u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            auto oracle = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(oracle.has_value());

            // Chunk 2 (chunk_x=2) with id 1 at local (0,0) -> global cell (256,0),
            // block (32,0): the first bit of the second occupancy word.
            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate chunk_update{.m_chunk_id = 2u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_chunks = {&chunk_update, 1u}, .m_revision = 1u}));
            PPR_TEST_ASSERT(not oracle->applyUpdate({.m_chunks = {&chunk_update, 1u}, .m_revision = 1u}));

            {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not source.encode(*encoder));
                fixture.submit(*encoder);
            }

            // 512x64 -> 64x8 blocks, words_per_row = 2, so 16 occupancy words.
            const auto occupancy = fixture.read<u32>(source.occupancy());
            PPR_TEST_ASSERT(occupancy.size() == 16u);
            PPR_TEST_ASSERT(occupancy[0] == 0u);
            PPR_TEST_ASSERT((occupancy[1] & 1u) == 1u);
            for (u32 block = 0u; block < 8u; ++block) {
                for (u32 word = 0u; word < 2u; ++word) {
                    PPR_TEST_ASSERT(occupancy[block * 2u + word] == oracle->occupancyWord(block, word));
                }
            }

            fixture.shutdown();
        };
    }

    // ------------------------------------------------------------------
    // temporal cascade field fixtures
    // ------------------------------------------------------------------

    namespace WorldLightFieldGpu {
        // 8-byte radiance sample: RGB (half) + scalar transmittance (half).
        struct RadianceHalf {
            u16 m_r{}, m_g{}, m_b{}, m_t{};
        };
        static_assert(sizeof(RadianceHalf) == 8u);

        [[nodiscard]] bool halfIsFinitePositive(const u16 value) noexcept {
            const u32 exponent = (value >> 10) & 0x1fu;
            return (value & 0x8000u) == 0u and exponent != 0x1fu and value != 0u;
        }

        // IEEE-754 half<->float. The composite test uploads the same half bytes
        // the GPU reads, so the CPU reference decodes them identically.
        [[nodiscard]] float halfToFloat(const u16 value) noexcept {
            const u32 sign = (value >> 15) & 1u;
            const u32 exponent = (value >> 10) & 0x1fu;
            const u32 mantissa = value & 0x3ffu;

            u32 bits;
            if (exponent == 0u) {
                if (mantissa == 0u) {
                    bits = sign << 31;
                } else {
                    u32 e = 127u - 15u + 1u;
                    u32 m = mantissa;
                    while ((m & 0x400u) == 0u) {
                        m <<= 1;
                        --e;
                    }
                    m &= 0x3ffu;
                    bits = (sign << 31) | (e << 23) | (m << 13);
                }
            } else if (exponent == 0x1fu) {
                bits = (sign << 31) | (0xffu << 23) | (mantissa << 13);
            } else {
                bits = (sign << 31) | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
            }
            return std::bit_cast<float>(bits);
        }

        [[nodiscard]] u16 floatToHalf(const float value) noexcept {
            const u32 bits = std::bit_cast<u32>(value);
            const u32 sign = (bits >> 16) & 0x8000u;
            const u32 exponent = (bits >> 23) & 0xffu;
            const u32 mantissa = bits & 0x7fffffu;

            if (exponent == 0xffu) {
                return static_cast<u16>(sign | 0x7c00u | (mantissa == 0u ? 0u : 0x200u));
            }

            const i32 unbiased = static_cast<i32>(exponent) - 127 + 15;
            if (unbiased >= 0x1f) {
                return static_cast<u16>(sign | 0x7c00u);
            }
            if (unbiased <= 0) {
                if (unbiased < -10) {
                    return static_cast<u16>(sign);
                }
                const u32 m = mantissa | 0x800000u;
                const i32 shift = 14 - unbiased;
                u32 rounded = m >> shift;
                const u32 remainder = m & ((1u << shift) - 1u);
                const u32 halfway = 1u << (shift - 1);
                if (remainder > halfway or (remainder == halfway and (rounded & 1u) != 0u)) {
                    ++rounded;
                }
                return static_cast<u16>(sign | rounded);
            }

            u32 rounded = mantissa >> 13;
            const u32 remainder = mantissa & 0x1fffu;
            if (remainder > 0x1000u or (remainder == 0x1000u and (rounded & 1u) != 0u)) {
                ++rounded;
            }
            u32 result = sign | (static_cast<u32>(unbiased) << 10) | rounded;
            if (rounded == 0x400u) {
                result = sign | ((static_cast<u32>(unbiased) + 1u) << 10);
            }
            return static_cast<u16>(result);
        }

        // Synthetic four-level hierarchy used by the composite/diffuse references
        // (levels 4+ stay zeroed with transmittance 0, so the composite stops at
        // level 3). Level l radiance (px,py,d) = (l + 0.25px, l + 0.25py, l + 0.25d)
        // and transmittance 0.9 - 0.2*l, so every level contributes a distinct
        // base and the transmittance chain spans the four finest levels.
        [[nodiscard]] std::vector<RadianceHalf> makeSyntheticField(const lighting::WorldLightPlan &plan) {
            std::vector<RadianceHalf> field(plan.m_radiance_samples, RadianceHalf{});
            for (u32 level = 0u; level < 4u; ++level) {
                const auto &L = plan.m_levels[level];
                const float base = float(level);
                const float transmittance = 0.9f - 0.2f * float(level);
                for (u32 py = 0u; py < L.m_probes_y; ++py) {
                    for (u32 px = 0u; px < L.m_probes_x; ++px) {
                        for (u32 d = 0u; d < L.m_directions; ++d) {
                            const u64 sample = L.m_sample_offset + (u64{py} * L.m_probes_x + px) * L.m_directions + d;
                            field[sample] = RadianceHalf{
                                .m_r = floatToHalf(base + 0.25f * float(px)),
                                .m_g = floatToHalf(base + 0.25f * float(py)),
                                .m_b = floatToHalf(base + 0.25f * float(d)),
                                .m_t = floatToHalf(transmittance)};
                        }
                    }
                }
            }
            return field;
        }

        // CPU oracle of worldLightComposite: per-level bilinear over probes,
        // angular bin from the direction (with the same boundary epsilon as the
        // shader), and the near-to-far transmittance composite. Decodes the same
        // half bytes the GPU reads.
        [[nodiscard]] std::array<float, 3> compositeReference(
            const lighting::WorldLightPlan &plan, const std::vector<RadianceHalf> &field, const float2 &position,
            const float2 &direction) {
            // The field origin is world (0,0) in WorldLightField::initialize; keep
            // the (position - origin) form the GPU uses so a non-zero origin stays
            // correct.
            constexpr float origin_x = 0.0f;
            constexpr float origin_y = 0.0f;

            std::array<float, 3> result{0.0f, 0.0f, 0.0f};
            float throughput = 1.0f;
            for (u32 level = 0u; level < plan.m_level_count; ++level) {
                const auto &L = plan.m_levels[level];
                const float spacing = float(L.m_spacing_cells);
                const float cell_metres = plan.m_cell_metres;

                const float uvx = (position.x - origin_x) / (cell_metres * spacing) - 0.5f;
                const float uvy = (position.y - origin_y) / (cell_metres * spacing) - 0.5f;
                const int base_x = int(std::floor(uvx));
                const int base_y = int(std::floor(uvy));
                const float fx = uvx - float(base_x);
                const float fy = uvy - float(base_y);

                float angle = std::atan2(direction.y, direction.x);
                if (angle < 0.0f)
                    angle += 6.28318530718f;
                const u32 d = u32(
                    std::clamp(
                        int(std::floor(angle * float(L.m_directions) / 6.28318530718f + 1e-3f)), 0,
                        int(L.m_directions) - 1));

                const auto probe = [&](const int px, const int py) -> std::pair<std::array<float, 3>, float> {
                    if (px < 0 or py < 0 or px >= int(L.m_probes_x) or py >= int(L.m_probes_y)) {
                        return {{0.0f, 0.0f, 0.0f}, 1.0f};
                    }
                    const u64 upx = static_cast<u64>(px);
                    const u64 upy = static_cast<u64>(py);
                    const u64 sample = L.m_sample_offset +
                                       (upy * u64{L.m_probes_x} + upx) * u64{L.m_directions} +
                                       u64{d};
                    const RadianceHalf &s = field[sample];
                    return {{halfToFloat(s.m_r), halfToFloat(s.m_g), halfToFloat(s.m_b)}, halfToFloat(s.m_t)};
                };

                const auto [r00, t00] = probe(base_x, base_y);
                const auto [r10, t10] = probe(base_x + 1, base_y);
                const auto [r01, t01] = probe(base_x, base_y + 1);
                const auto [r11, t11] = probe(base_x + 1, base_y + 1);

                const auto lerp3 = [](const std::array<float, 3> &a, const std::array<float, 3> &b, const float t) {
                    return std::array<float, 3>{
                        a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
                };
                const std::array<float, 3> r0 = lerp3(r00, r10, fx);
                const std::array<float, 3> r1 = lerp3(r01, r11, fx);
                const float t0 = t00 + (t10 - t00) * fx;
                const float t1 = t01 + (t11 - t01) * fx;
                const std::array<float, 3> radiance = lerp3(r0, r1, fy);
                const float transmittance = t0 + (t1 - t0) * fy;

                result[0] += throughput * radiance[0];
                result[1] += throughput * radiance[1];
                result[2] += throughput * radiance[2];
                throughput *= transmittance;
            }
            return result;
        }

        // CPU oracle of worldLightDiffuse: the deterministic 8-direction set with
        // cosine weights from the horizontal tilt; a flat (+Z) normal is isotropic.
        [[nodiscard]] std::array<float, 3> diffuseReference(
            const lighting::WorldLightPlan &plan, const std::vector<RadianceHalf> &field, const float2 &position,
            const float3 &normal) {
            const float tilt = std::sqrt(normal.x * normal.x + normal.y * normal.y);

            std::array<float, 3> result{0.0f, 0.0f, 0.0f};
            float weight_sum = 0.0f;
            for (u32 i = 0u; i < 8u; ++i) {
                const float angle = (2.0f * float(i) + 0.5f) * 6.28318530718f / 16.0f;
                const float2 dir{std::cos(angle), std::sin(angle)};

                float weight = 1.0f;
                if (tilt > 1e-6f) {
                    weight = std::max(dir.x * (normal.x / tilt) + dir.y * (normal.y / tilt), 0.0f);
                }
                const auto c = compositeReference(plan, field, position, dir);
                result[0] += c[0] * weight;
                result[1] += c[1] * weight;
                result[2] += c[2] * weight;
                weight_sum += weight;
            }
            if (weight_sum > 0.0f) {
                result[0] /= weight_sum;
                result[1] /= weight_sum;
                result[2] /= weight_sum;
            }
            return result;
        }

        struct Fixture {
            safe_ptr<IShaderService> m_shaders{IShaderService::get()};
            safe_ptr<IRhiService> m_rhi{IRhiService::get()};
            rhi::ComPtr<rhi::ICommandQueue> m_queue;
            FrameFence m_fence;
            lighting::WorldLightSourceGpu m_source;
            lighting::WorldLightField m_field;
            GridPass m_grid{};
            bool m_grid_ready{};

            void initialize(const u32 width, const u32 height) {
                PPR_TEST_ASSERT(not m_shaders->initialize());
                const auto setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
                const bool validation = setting and *setting == "1";
                m_rhi->setCoreValidation(true);
                m_rhi->setGpuAssistedValidation(validation);
#ifdef _WIN32
                constexpr auto backend = rhi::DeviceType::D3D12;
#else
                constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
                PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
                auto &device = m_rhi->getDevice();
                PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));
                PPR_TEST_ASSERT(not m_fence.initialize(device));

                const auto plan = lighting::planWorldLight(
                    {.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
                PPR_TEST_ASSERT(plan.has_value());
                // One opaque emissive material (palette index 0 -> cell id 1).
                const lighting::WorldLightMaterial materials[] = {{.m_kind = 1u, .m_emission = {1.0f, 1.0f, 1.0f}}};
                PPR_TEST_ASSERT(
                    not m_source.initialize(*m_rhi, *m_shaders, *plan, materials, std::filesystem::current_path()));
                PPR_TEST_ASSERT(not m_field.initialize(*m_rhi, *m_shaders, *plan, std::filesystem::current_path()));
            }

            void submit(rhi::ICommandEncoder &encoder) {
                rhi::ComPtr<rhi::ICommandBuffer> commands;
                PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));

                rhi::ICommandBuffer *pointer = commands.get();
                rhi::IFence *fence = m_fence.fence();
                const auto value = m_fence.nextValue();
                const u64 raw = value;
                rhi::SubmitDesc description{};
                description.commandBuffers = &pointer;
                description.commandBufferCount = 1u;
                description.signalFences = &fence;
                description.signalFenceValues = &raw;
                description.signalFenceCount = 1u;
                PPR_TEST_ASSERT(not make_error_code(m_queue->submit(description)));
                m_fence.commitSubmitted(value);
                PPR_TEST_ASSERT(not m_fence.waitFor(value));
            }

            template<typename T>
            std::vector<T> read(rhi::IBuffer *const source) {
                rhi::BufferDesc description{};
                description.size = source->getDesc().size;
                description.elementSize = 0u;
                description.memoryType = rhi::MemoryType::ReadBack;
                description.usage = rhi::BufferUsage::CopyDestination;
                description.defaultState = rhi::ResourceState::CopyDestination;
                description.label = "WorldLightFieldGpu readback";
                rhi::ComPtr<rhi::IBuffer> copy;
                PPR_TEST_ASSERT(
                    not make_error_code(m_rhi->getDevice().createBuffer(description, nullptr, copy.writeRef())));

                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                encoder->setBufferState(source, rhi::ResourceState::CopySource);
                encoder->copyBuffer(copy, 0u, source, 0u, source->getDesc().size);
                encoder->setBufferState(source, rhi::ResourceState::UnorderedAccess);
                submit(*encoder);

                std::vector<T> result(source->getDesc().size / sizeof(T));
                void *mapped{};
                PPR_TEST_ASSERT(
                    not make_error_code(m_rhi->getDevice().mapBuffer(copy, rhi::CpuAccessMode::Read, &mapped)));
                std::memcpy(result.data(), mapped, result.size() * sizeof(T));
                PPR_TEST_ASSERT(not make_error_code(m_rhi->getDevice().unmapBuffer(copy)));
                return result;
            }

            template<typename T>
            void write(rhi::IBuffer *const destination, const std::vector<T> &data) {
                const u64 bytes = data.size() * sizeof(T);
                rhi::BufferDesc description{};
                description.size = bytes;
                description.elementSize = 0u;
                description.memoryType = rhi::MemoryType::Upload;
                description.usage = rhi::BufferUsage::CopySource;
                description.defaultState = rhi::ResourceState::CopySource;
                description.label = "WorldLightFieldGpu upload";
                rhi::ComPtr<rhi::IBuffer> staging;
                PPR_TEST_ASSERT(
                    not make_error_code(m_rhi->getDevice().createBuffer(description, nullptr, staging.writeRef())));

                void *mapped{};
                PPR_TEST_ASSERT(not make_error_code(
                    m_rhi->getDevice().mapBuffer(staging.get(), rhi::CpuAccessMode::Write, &mapped)));
                std::memcpy(mapped, data.data(), bytes);
                PPR_TEST_ASSERT(not make_error_code(m_rhi->getDevice().unmapBuffer(staging.get())));

                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                encoder->setBufferState(destination, rhi::ResourceState::CopyDestination);
                encoder->copyBuffer(destination, 0u, staging.get(), 0u, bytes);
                encoder->setBufferState(destination, rhi::ResourceState::UnorderedAccess);
                submit(*encoder);
            }

            std::array<float, 3> evaluate(
                const float2 &position, const float2 &direction, const float3 &normal, const float roughness,
                const lighting::WorldLightField::EvaluateMode mode) {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not m_field.evaluate(*encoder, position, direction, normal, roughness, mode));
                submit(*encoder);

                const auto out = read<float>(m_field.evaluateOutput());
                PPR_TEST_ASSERT(out.size() == 3u);
                return {out[0], out[1], out[2]};
            }

            void encodeSource() {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not m_source.encode(*encoder));
                submit(*encoder);
            }

            void updateField(const u32 budget) {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not m_field.update(*encoder, m_source, 1.0f / 60.0f, budget));
                submit(*encoder);
            }

            void runUpdates(const u32 count, const u32 budget) {
                for (u32 index = 0u; index < count; ++index) {
                    updateField(budget);
                }
            }

            // Lazy GridPass for the shading test; the field-only tests never pay
            // for the raster pipeline.
            void initializeGrid() {
                if (m_grid_ready) {
                    return;
                }
                PPR_TEST_ASSERT(not m_grid.initialize(*m_rhi, *m_shaders, std::filesystem::current_path()));
                m_grid_ready = true;
            }

            void shutdown() {
                if (m_grid_ready) {
                    PPR_TEST_ASSERT(not m_grid.shutdown());
                    m_grid_ready = false;
                }
                if (m_queue and make_error_code(m_queue->waitOnHost()))
                    hal::fatalExit("world-light-field-test-quiescence-unknown");

                PPR_TEST_ASSERT(not m_field.shutdown());
                PPR_TEST_ASSERT(not m_source.shutdown());
                PPR_TEST_ASSERT(not m_fence.shutdown());
                m_queue.setNull();
                PPR_TEST_ASSERT(not m_rhi->shutdown());
                PPR_TEST_ASSERT(not m_shaders->shutdown());
            }
        };

        PPR_UNIT_TEST (field_single_emitter_converges) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            const u32 budget = static_cast<u32>(plan->m_levels[0].m_sample_count); // one full level

            // One opaque emissive cell at (0,0) via chunk 0.
            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 1u}));
            fixture.encodeSource();

            // Level 0, probe (0,0), direction 2 (down-left) points at cell (0,0).
            const u64 nearby_sample = 2u;
            const u64 nearby_word = nearby_sample * 4u;

            fixture.runUpdates(8u, budget);
            const auto count8 = fixture.read<u32>(field.state());
            const u32 nearby_count_8 = count8[nearby_sample];

            fixture.runUpdates(8u, budget);
            const auto count16 = fixture.read<u32>(field.state());
            const auto radiance = fixture.read<u16>(field.view().m_radiance);
            const u32 nearby_count_16 = count16[nearby_sample];

            // Monotonic per-direction sample count, and finite positive radiance.
            PPR_TEST_ASSERT(nearby_count_16 >= nearby_count_8);
            PPR_TEST_ASSERT(nearby_count_16 > 0u);
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[nearby_word + 0u]));
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[nearby_word + 1u]));
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[nearby_word + 2u]));

            // The cursor advanced and wrapped within the field after 16 updates.
            PPR_TEST_ASSERT(field.cycle() >= 1u);
            PPR_TEST_ASSERT(field.cursor() < plan->m_radiance_samples);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (field_dirty_update_reprocesses_in_same_frame) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            const u32 budget = static_cast<u32>(plan->m_levels[0].m_sample_count);

            // Level 0 probe (0,0) direction 2 is near the emitter; probe (100,100)
            // direction 0 is distant (outside the dirty chunk).
            const u64 nearby_sample = 2u;
            const u64 nearby_word = nearby_sample * 4u;
            const u64 distant_sample = (100u * 128u + 100u) * 4u + 0u;

            // Warm the whole field with 16 vacuum updates.
            fixture.runUpdates(16u, budget);
            const auto before = fixture.read<u32>(field.state());
            const u32 distant_before = before[distant_sample];
            PPR_TEST_ASSERT(distant_before > 0u);

            // Turn the emitter on at cell (0,0), bumping the revision.
            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate on{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_chunks = {&on, 1u}, .m_revision = 1u}));
            fixture.encodeSource();

            // One field update resets and immediately reprocesses the dirty probe,
            // so the edit is visible in the same frame.
            fixture.updateField(budget);
            const auto after_count = fixture.read<u32>(field.state());
            const auto after_radiance = fixture.read<u16>(field.view().m_radiance);

            PPR_TEST_ASSERT(after_count[nearby_sample] == 1u);
            PPR_TEST_ASSERT(halfIsFinitePositive(after_radiance[nearby_word + 0u]));
            PPR_TEST_ASSERT(after_count[distant_sample] == distant_before);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (field_reset_bounds_rect_tail) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            const u32 budget = static_cast<u32>(plan->m_levels[0].m_sample_count);

            // Level 0 probe (5,6) direction 0 sits one row below the dirty probe
            // (5,5) and must keep its history.
            const u64 below_sample = (6u * 128u + 5u) * 4u + 0u;

            fixture.runUpdates(16u, budget);
            const auto before = fixture.read<u32>(field.state());
            const u32 below_before = before[below_sample];
            PPR_TEST_ASSERT(below_before > 0u);

            // A single-cell edit: a particle at cell (10,10) dirties a 1x1 probe
            // rect whose count (1) is not a multiple of the 64-thread group.
            const lighting::WorldLightMatterParticle particle{
                .m_cell = 10u * width + 10u, .m_fill = 1.0f, .m_material = 0u};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_particles = {&particle, 1u}, .m_revision = 1u}));
            fixture.encodeSource();

            fixture.updateField(budget);
            const auto after = fixture.read<u32>(field.state());

            // The reset tail must not clear the probe just outside the rect.
            PPR_TEST_ASSERT(after[below_sample] == below_before);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (field_update_respects_budget_boundary) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &field = fixture.m_field;

            // A budget that is not a multiple of the 64-thread group size.
            const u32 budget = 100u;
            fixture.updateField(budget);
            const auto count = fixture.read<u32>(field.state());

            // Only the first `budget` samples are traced; the tail group must not
            // leak past the slice.
            PPR_TEST_ASSERT(count[99u] == 1u);
            PPR_TEST_ASSERT(count[100u] == 0u);
            PPR_TEST_ASSERT(count[127u] == 0u);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (field_dense_dda_hits_diagonal_emitter) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            const u32 budget = static_cast<u32>(plan->m_levels[0].m_sample_count);

            // Level 0 probe (3,3) direction 0 (up-right) passes diagonally through
            // block (0,0) and hits the single opaque cell (8,8) in block (1,1).
            const u64 sample = (3u * 128u + 3u) * 4u + 0u;
            const u64 word = sample * 4u;

            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[8u * 128u + 8u] = 1u;
            const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 1u}));
            fixture.encodeSource();

            fixture.runUpdates(8u, budget);
            const auto radiance = fixture.read<u16>(field.view().m_radiance);

            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[word + 0u]));
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[word + 1u]));
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[word + 2u]));

            fixture.shutdown();
        };

        PPR_UNIT_TEST (field_is_camera_independent) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            // The update() interface has no camera input; two identical field
            // sequences produce identical bytes.
            const auto run = [&]() -> std::vector<u16> {
                Fixture fixture;
                fixture.initialize(width, height);

                const auto plan = lighting::planWorldLight(
                    {.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
                PPR_TEST_ASSERT(plan.has_value());
                const u32 budget = static_cast<u32>(plan->m_levels[0].m_sample_count);

                std::vector<u16> chunk(128u * 128u, 0u);
                chunk[0] = 1u;
                const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
                PPR_TEST_ASSERT(not fixture.m_source.applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 1u}));
                fixture.encodeSource();
                fixture.runUpdates(16u, budget);

                const auto bytes = fixture.read<u16>(fixture.m_field.view().m_radiance);
                fixture.shutdown();
                return bytes;
            };

            PPR_TEST_ASSERT(run() == run());
        };

        PPR_UNIT_TEST (composite_matches_cpu_reference) {
            constexpr u32 width = 64u;
            constexpr u32 height = 64u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());

            // Upload the synthetic hierarchy into the current radiance buffer, then
            // compare the GPU composite to the CPU oracle at a position between
            // probes and a direction away from any angular bin boundary.
            const std::vector<RadianceHalf> synthetic = makeSyntheticField(*plan);
            fixture.write(field.view().m_radiance, synthetic);

            const float2 position{2.3f, 3.7f};
            const float2 direction{std::cos(1.0f), std::sin(1.0f)};

            const std::array<float, 3> expected = compositeReference(*plan, synthetic, position, direction);
            const std::array<float, 3> actual = fixture.evaluate(
                position, direction, float3{0.0f, 0.0f, 1.0f}, 0.0f,
                lighting::WorldLightField::EvaluateMode::kComposite);

            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::isfinite(actual[channel]));
                PPR_TEST_ASSERT(std::abs(actual[channel] - expected[channel]) <= 1e-4f);
            }

            fixture.shutdown();
        };

        PPR_UNIT_TEST (diffuse_plus_z_isotropic_and_perturbed_finite) {
            constexpr u32 width = 64u;
            constexpr u32 height = 64u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());

            const std::vector<RadianceHalf> synthetic = makeSyntheticField(*plan);
            fixture.write(field.view().m_radiance, synthetic);

            const float2 position{2.3f, 3.7f};
            const float2 direction{1.0f, 0.0f};

            // +Z normal: the near-isotropic term (average of the direction set).
            const std::array<float, 3> plus_z = fixture.evaluate(
                position, direction, float3{0.0f, 0.0f, 1.0f}, 0.0f, lighting::WorldLightField::EvaluateMode::kDiffuse);
            const std::array<float, 3> plus_z_ref = diffuseReference(
                *plan, synthetic, position, float3{0.0f, 0.0f, 1.0f});
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::isfinite(plus_z[channel]));
                PPR_TEST_ASSERT(std::abs(plus_z[channel] - plus_z_ref[channel]) <= 1e-4f);
            }

            // A tilted normal changes the result and never produces NaN.
            const std::array<float, 3> perturbed = fixture.evaluate(
                position, direction, float3{0.5f, -0.3f, 0.8f}, 0.0f,
                lighting::WorldLightField::EvaluateMode::kDiffuse);
            const std::array<float, 3> perturbed_ref = diffuseReference(
                *plan, synthetic, position, float3{0.5f, -0.3f, 0.8f});
            bool changed = false;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::isfinite(perturbed[channel]));
                PPR_TEST_ASSERT(std::abs(perturbed[channel] - perturbed_ref[channel]) <= 1e-4f);
                changed = changed or std::abs(perturbed[channel] - plus_z[channel]) > 1e-4f;
            }
            PPR_TEST_ASSERT(changed);

            fixture.shutdown();
        };

        PPR_UNIT_TEST (specular_vertical_reflection_and_roughness_finite) {
            constexpr u32 width = 64u;
            constexpr u32 height = 64u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());

            const std::vector<RadianceHalf> synthetic = makeSyntheticField(*plan);
            fixture.write(field.view().m_radiance, synthetic);

            const float2 position{2.3f, 3.7f};

            // A vertical (zero horizontal) reflection has no in-plane direction to
            // sample: the specular term is finite and exactly zero, never NaN.
            const std::array<float, 3> vertical = fixture.evaluate(
                position, float2{0.0f, 0.0f}, float3{0.0f, 0.0f, 1.0f}, 0.0f,
                lighting::WorldLightField::EvaluateMode::kSpecular);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::isfinite(vertical[channel]));
                PPR_TEST_ASSERT(vertical[channel] == 0.0f);
            }

            // Zero roughness returns the composite at the reflection direction.
            const float2 reflection{1.0f, 0.0f};
            const std::array<float, 3> sharp = fixture.evaluate(
                position, reflection, float3{0.0f, 0.0f, 1.0f}, 0.0f,
                lighting::WorldLightField::EvaluateMode::kSpecular);
            const std::array<float, 3> sharp_ref = compositeReference(*plan, synthetic, position, reflection);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::isfinite(sharp[channel]));
                PPR_TEST_ASSERT(std::abs(sharp[channel] - sharp_ref[channel]) <= 1e-4f);
            }

            // Positive roughness widens the lobe but stays finite.
            const std::array<float, 3> rough = fixture.evaluate(
                position, reflection, float3{0.0f, 0.0f, 1.0f}, 0.5f,
                lighting::WorldLightField::EvaluateMode::kSpecular);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::isfinite(rough[channel]));
            }

            fixture.shutdown();
        };

        PPR_UNIT_TEST (grid_shading_uses_field) {
            constexpr u32 width = 64u;
            constexpr u32 height = 64u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &field = fixture.m_field;
            auto &device = fixture.m_rhi->getDevice();

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 1.0f});
            PPR_TEST_ASSERT(plan.has_value());

            // Uniform field: every radiance sample carries the same white radiance
            // and unit transmittance, so the diffuse term is isotropic and the
            // specular term equals the field value for any in-plane reflection.
            std::vector<RadianceHalf> uniform(plan->m_radiance_samples, RadianceHalf{});
            for (auto &sample: uniform) {
                sample = RadianceHalf{
                    .m_r = floatToHalf(10.0f),
                    .m_g = floatToHalf(10.0f),
                    .m_b = floatToHalf(10.0f),
                    .m_t = floatToHalf(1.0f)};
            }
            fixture.write(field.view().m_radiance, uniform);

            fixture.initializeGrid();
            auto &grid = fixture.m_grid;

            // Two materials: id 1 is flat, id 2 has a +X-tilted normal map. Both
            // share white albedo, zero emission, unit roughness and IOR=3 so the
            // field's specular term separates them.
            GridStagedFrame stage{};
            stage.m_full_refresh = stage.m_reset_caches = stage.m_has_catalog = true;
            stage.m_cell_size_m = 1.0f;
            stage.m_boundary_material_id = 1u;
            stage.m_content_fingerprint = 13u;

            GridTerrainVisual flat{};
            flat.m_material_id = 1u;
            flat.m_color_rgba = 0xffffffffu;
            flat.m_texture_index = 0u;
            flat.m_transport_model = kGridTransportOpaqueV1;
            flat.m_metallic = 0.0f;
            flat.m_roughness = 0.0f;
            flat.m_ior = 3.0f;
            stage.m_terrain_visuals.push_back(flat);

            GridTerrainVisual tilted = flat;
            tilted.m_material_id = 2u;
            tilted.m_normal_texture_index = 1u;
            stage.m_terrain_visuals.push_back(tilted);

            // White albedo (image 0) and a +X-tilted normal map (image 1):
            // RGBA8 (0.75, 0.5, 1.0, 1.0) decodes to (0.5, 0, 1) -> +X tilt.
            const auto image = [](const std::array<std::byte, 4u> bytes) {
                mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(bytes.size());
                PPR_TEST_ASSERT(not storage.materialize());
                const auto writable = storage.getMutableData();
                PPR_TEST_ASSERT(writable.has_value());
                std::ranges::copy(bytes, writable->begin());

                mem::SharedBuffer frozen;
                PPR_TEST_ASSERT(not storage.moveToShared(&frozen));

                image::ImageAsset result{};
                result.m_width = result.m_height = 1u;
                result.m_storage = frozen;
                result.m_subresources.push_back({frozen.subspan(0u, 4u), 4u, 4u});
                return result;
            };
            stage.m_terrain_images.push_back(image({std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}}));
            stage.m_terrain_images.push_back(image({std::byte{191}, std::byte{128}, std::byte{255}, std::byte{255}}));

            for (u32 layer = 0u; layer < 4u; ++layer) {
                stage.m_background_layers[layer].m_texture_index = 0u;
                stage.m_background_layers[layer].m_order = layer;
            }

            // One chunk with two cells: material 1 at (2,2), material 2 at (6,2).
            stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridStagedUpload upload{};
            upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            upload.m_cell_materials[2u * 128u + 2u] = 1u;
            upload.m_cell_materials[2u * 128u + 6u] = 2u;
            stage.m_uploads.push_back(std::move(upload));

            PPR_TEST_ASSERT(not grid.stageWorldCandidate(7u, 11u, stage));
            bool candidate_ready = false;
            for (u32 advance = 0u; advance < 8u; ++advance) {
                const auto ready = grid.advanceWorldCandidate(device, fixture.m_fence.completed());
                PPR_TEST_ASSERT(ready.has_value());
                if (*ready) {
                    candidate_ready = true;
                    break;
                }
            }
            PPR_TEST_ASSERT(candidate_ready);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(7u, 11u, 13u));

            // Orthographic camera viewing the 8x8-cell region around the two cells.
            constexpr u32 target_edge = 128u;
            Camera camera{ECameraProjection::orthographic};
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{0.0f, 0.0f, -0.5f};
            model.m_ortho_scale = 1.0f / 16.0f;
            camera.updateModel(
                TimeSpan{}, model,
                Viewport{PixelRect{int2{0, 0}, int2{int(target_edge), int(target_edge)}}, ViewportLayout{}});
            const auto snapshot = camera.getSnapshot();

            // HDR render target.
            rhi::TextureDesc target_desc{};
            target_desc.type = rhi::TextureType::Texture2D;
            target_desc.size = {.width = target_edge, .height = target_edge, .depth = 1u};
            target_desc.format = rhi::Format::RGBA32Float;
            target_desc.memoryType = rhi::MemoryType::DeviceLocal;
            target_desc.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
            target_desc.defaultState = rhi::ResourceState::RenderTarget;
            rhi::ComPtr<rhi::ITexture> target;
            rhi::ComPtr<rhi::ITextureView> target_view;
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(target_desc, nullptr, target.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(target->getDefaultView(target_view.writeRef())));

            const auto viewport = rhi::Viewport::fromSize(float(target_edge), float(target_edge));
            const auto scissor = rhi::ScissorRect::fromSize(target_edge, target_edge);

            const auto render = [&](const lighting::WorldLightFieldView view) {
                PPR_TEST_ASSERT(not grid.setWorldLightField(view));
                PPR_TEST_ASSERT(not grid.update(
                    TimeSpan{}, snapshot, false, GridPass::ReceiverContinuation::nearest,
                    GridPass::ReceiverResponse::flat, GridPass::BackgroundResponse::unlit));

                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                const PreRenderContext pre{device, *encoder, fixture.m_fence.completed(), {}};
                PPR_TEST_ASSERT(not grid.prepareGpuVisibility(pre));

                encoder->setTextureState(
                    target, rhi::SubresourceRange{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u},
                    rhi::ResourceState::RenderTarget);
                rhi::RenderPassColorAttachment attachment{};
                attachment.view = target_view;
                attachment.clearValue[0] = attachment.clearValue[1] = attachment.clearValue[2] =
                    attachment.clearValue[3] = 0.0f;
                rhi::RenderPassDesc pass_desc{};
                pass_desc.colorAttachments = &attachment;
                pass_desc.colorAttachmentCount = 1u;
                auto *const pass = encoder->beginRenderPass(pass_desc);
                PPR_TEST_ASSERT(pass != nullptr);
                {
                    PPR_DEFER {
                        pass->end();
                    };
                    RenderPipelineSignature signature{};
                    signature.m_color_formats[0] = rhi::Format::RGBA32Float;
                    signature.m_color_format_count = 1u;
                    const RenderPipelineKey key{signature};
                    const DrawContext draw{
                        device, *pass, key, viewport, scissor, int2{int(target_edge), int(target_edge)},
                        fixture.m_fence.completed(), {}};
                    PPR_TEST_ASSERT(not grid.renderGpuIndirect(draw));
                }

                rhi::ComPtr<rhi::ICommandBuffer> commands;
                PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
                rhi::ICommandBuffer *pointer = commands.get();
                rhi::IFence *signal = fixture.m_fence.fence();
                const auto value = fixture.m_fence.nextValue();
                const u64 raw_value = value;
                rhi::SubmitDesc submit{};
                submit.commandBuffers = &pointer;
                submit.commandBufferCount = 1u;
                submit.signalFences = &signal;
                submit.signalFenceValues = &raw_value;
                submit.signalFenceCount = 1u;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(submit)));
                fixture.m_fence.commitSubmitted(value);
                grid.notifyFrameSubmitted(value);
                PPR_TEST_ASSERT(not fixture.m_fence.waitFor(value));
            };

            const auto readback = [&]() {
                shader::ComPtr<ISlangBlob> blob;
                rhi::SubresourceLayout layout{};
                PPR_TEST_ASSERT(not make_error_code(device.readTexture(target, 0u, 0u, blob.writeRef(), &layout)));
                const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());

                std::vector<std::array<float, 4u>> pixels(std::size_t(target_edge) * target_edge);
                for (u32 y = 0u; y < target_edge; ++y) {
                    for (u32 x = 0u; x < target_edge; ++x) {
                        std::array<float, 4u> value{};
                        std::memcpy(value.data(), source + y * layout.rowPitch + x * sizeof(value), sizeof(value));
                        pixels[y * target_edge + x] = value;
                    }
                }
                return pixels;
            };

            // Lit pass, then read the indirect draw arguments (one quad per
            // 128² chunk) and the HDR colour.
            render(field.view());
            {
                const auto [args_buffer, ids_buffer] = grid.visibilityBuffersForTest();
                PPR_TEST_ASSERT(args_buffer != nullptr and ids_buffer != nullptr);
                rhi::IndirectDrawArguments args{};
                PPR_TEST_ASSERT(not make_error_code(device.readBuffer(args_buffer, 0u, sizeof(args), &args)));
                PPR_TEST_ASSERT(args.vertexCountPerInstance == GridPass::kGridTileVertexCount);
                PPR_TEST_ASSERT(args.instanceCount == 1u);
                PPR_TEST_ASSERT(args.startVertexLocation == 0u and args.startInstanceLocation == 0u);
            }
            const auto lit = readback();

            // Unlit pass: an empty view selects the existing path.
            render({});
            const auto unlit = readback();

            // Collect the distinct non-zero colours (with a small tolerance for
            // any per-pixel interpolation noise), proving finiteness.
            const auto collect = [](const std::vector<std::array<float, 4u>> &pixels) {
                std::vector<std::array<float, 3u>> colours;
                for (const auto &pixel: pixels) {
                    PPR_TEST_ASSERT(std::isfinite(pixel[0]) and std::isfinite(pixel[1]) and std::isfinite(pixel[2]));
                    if (pixel[0] == 0.0f and pixel[1] == 0.0f and pixel[2] == 0.0f) {
                        continue;
                    }
                    const std::array<float, 3u> colour{pixel[0], pixel[1], pixel[2]};
                    bool seen = false;
                    for (const auto &other: colours) {
                        if (std::abs(colour[0] - other[0]) < 1e-3f and
                            std::abs(colour[1] - other[1]) < 1e-3f and
                            std::abs(colour[2] - other[2]) < 1e-3f) {
                            seen = true;
                            break;
                        }
                    }
                    if (not seen) {
                        colours.push_back(colour);
                    }
                }
                return colours;
            };

            const auto lit_colours = collect(lit);
            const auto unlit_colours = collect(unlit);

            // The lit field is finite, non-zero, and the two cells (flat vs
            // normal-mapped) shade differently.
            PPR_TEST_ASSERT(not lit_colours.empty());
            PPR_TEST_ASSERT(lit_colours.size() >= 2u);

            // Lit differs from unlit.
            PPR_TEST_ASSERT(lit_colours != unlit_colours);

            // Release the test-owned target before the RHI teardown.
            target_view.setNull();
            target.setNull();

            fixture.shutdown();
        };
    }
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // world light field contracts suite
    // ------------------------------------------------------------------

    const UnitTest world_light_field_contracts = UnitTest::Named("world_light_field") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::WorldLightFieldTests::plan_1024_and_2048_use_spacing_two_and_equal_level_samples,
            detail::WorldLightFieldTests::plan_rejects_zero_dimensions_nonfinite_cell_size_and_excess_levels,
            detail::WorldLightFieldTests::source_applies_dirty_chunks_and_preserves_other_cells,
            detail::WorldLightFieldTests::source_empty_update_keeps_revision_and_dirty_rect,
            detail::WorldLightFieldTests::source_rejects_out_of_range_chunk,
            detail::WorldLightFieldTests::source_applies_overlay_and_bumps_revision,
            detail::WorldLightFieldTests::source_derives_occupancy_word_for_traversal,
            detail::WorldLightFieldTests::source_overlay_occupies_and_clears_occupancy,
            detail::WorldLightGpu::gpu_source_matter_overlay,
            detail::WorldLightGpu::gpu_source_over_capacity_update_preserves_state,
            detail::WorldLightGpu::gpu_source_second_update_replaces_pending_particles,
            detail::WorldLightGpu::gpu_source_multiword_occupancy,
            detail::WorldLightFieldGpu::field_single_emitter_converges,
            detail::WorldLightFieldGpu::field_dirty_update_reprocesses_in_same_frame,
            detail::WorldLightFieldGpu::field_reset_bounds_rect_tail,
            detail::WorldLightFieldGpu::field_update_respects_budget_boundary,
            detail::WorldLightFieldGpu::field_dense_dda_hits_diagonal_emitter,
            detail::WorldLightFieldGpu::field_is_camera_independent,
            detail::WorldLightFieldGpu::composite_matches_cpu_reference,
            detail::WorldLightFieldGpu::diffuse_plus_z_isotropic_and_perturbed_finite,
            detail::WorldLightFieldGpu::specular_vertical_reflection_and_roughness_finite,
            detail::WorldLightFieldGpu::grid_shading_uses_field,
        });
    };

    const UnitTest &world_light_fieldTests() noexcept {
        return world_light_field_contracts;
    }
}
