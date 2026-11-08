module;
#include "pP/UnitTest.h"
#include <cstring>

module engine.tests.app;

import engine.core;
import engine.app;
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

        struct Fixture {
            safe_ptr<IShaderService> m_shaders{IShaderService::get()};
            safe_ptr<IRhiService> m_rhi{IRhiService::get()};
            rhi::ComPtr<rhi::ICommandQueue> m_queue;
            FrameFence m_fence;
            lighting::WorldLightSourceGpu m_source;
            lighting::WorldLightField m_field;

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

            void shutdown() {
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
        });
    };

    const UnitTest &world_light_fieldTests() noexcept {
        return world_light_field_contracts;
    }
}
