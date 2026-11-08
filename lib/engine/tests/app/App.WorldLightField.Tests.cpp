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
                const lighting::WorldLightMaterial materials[] = {{.m_kind = 1u}};
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

            // One opaque particle at cell (4,4) with coverage 1.
            const lighting::WorldLightOverlayCell particle{.m_cell = 4u * width + 4u, .m_coverage = 1.0f};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_overlay = {&particle, 1u}, .m_revision = 3u}));
            PPR_TEST_ASSERT(not oracle->applyUpdate({.m_overlay = {&particle, 1u}, .m_revision = 3u}));

            {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not source.encode(*encoder));
                fixture.submit(*encoder);
            }

            // Cell ids: id 1 at (0,0), vacuum elsewhere.
            const auto ids = fixture.read<u32>(source.cellIds());
            PPR_TEST_ASSERT(ids.size() == u64{width} * height);
            PPR_TEST_ASSERT(ids[0] == 1u);
            PPR_TEST_ASSERT(ids[1] == 0u);

            // Overlay: coverage 1 at (4,4).
            const auto overlay = fixture.read<OverlayRow>(source.overlay());
            PPR_TEST_ASSERT(overlay[4u * width + 4u].coverage == 1.0f);
            PPR_TEST_ASSERT(overlay[0].coverage == 0.0f);

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
        });
    };

    const UnitTest &world_light_fieldTests() noexcept {
        return world_light_field_contracts;
    }
}
