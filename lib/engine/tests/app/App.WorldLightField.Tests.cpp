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

        PPR_UNIT_TEST (producer_stages_only_dirty_chunks_and_advances_revision) {
            // An ordinary edit: three resident chunks, but only chunk 1 is dirty
            // and uploaded. The producer must stage exactly that dirty set, never
            // the whole resident world.
            GridStagedFrame stage{};
            stage.m_cell_size_m = 1.0f;
            stage.m_full_refresh = false;
            stage.m_temporal_sky_hour = 7;

            stage.m_tiles.push_back(GridTileSubmission{.m_chunk_id = 0u});
            stage.m_tiles.push_back(GridTileSubmission{.m_chunk_id = 32u});
            stage.m_tiles.push_back(GridTileSubmission{.m_chunk_id = 1u});

            GridStagedUpload upload{};
            upload.m_chunk_id = 32u; // sim chunkIndexOf: pos {0,1} in the max 32-stride.
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            upload.m_cell_materials[0u] = 1u;
            stage.m_uploads.push_back(std::move(upload));

            auto particles = std::make_shared<Array<GridParticleInstance>>();
            particles->push_back({u64{1}, 2.5, 2.5, 0.0, 1.0, 300.0, 3u});
            particles->push_back({u64{2}, 6.5, 6.5, 0.0, 1.0, 300.0, 3u});
            stage.m_particle_batches.push_back(GridParticleBatch{0u, particles});

            const lighting::WorldLightSourceUpdateStorage storage = lighting::buildWorldLightSourceUpdate(
                stage, 256u, 5u, 7);

            // The max-stride chunk id 32 ({0,1}) becomes the dense id 2 over the
            // 256-edge world's two chunks per edge.
            PPR_TEST_ASSERT(storage.m_update.m_chunks.size() == 1u);
            PPR_TEST_ASSERT(storage.m_update.m_chunks[0].m_chunk_id == 2u);
            PPR_TEST_ASSERT(storage.m_update.m_chunks[0].m_cell_ids.size() == GridPass::kCellsPerChunk);
            PPR_TEST_ASSERT(storage.m_update.m_chunks[0].m_cell_ids[0] == 1u);

            PPR_TEST_ASSERT(storage.m_update.m_particles.size() == 2u);
            PPR_TEST_ASSERT(storage.m_update.m_particles[0].m_cell == 2u * 256u + 2u);
            PPR_TEST_ASSERT(storage.m_update.m_particles[1].m_cell == 6u * 256u + 6u);
            PPR_TEST_ASSERT(storage.m_update.m_particles[0].m_material == 2u);

            PPR_TEST_ASSERT(storage.m_update.m_revision == 5u);
            PPR_TEST_ASSERT(storage.m_update.m_sky_hour == 7);

            // The update advances a dense source's revision.
            const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 1.0f});
            PPR_TEST_ASSERT(plan.has_value());
            auto source = lighting::WorldLightSource::create(*plan, {});
            PPR_TEST_ASSERT(source.has_value());
            PPR_TEST_ASSERT(not source->applyUpdate(storage.m_update));
            PPR_TEST_ASSERT(source->revision() == 5u);
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

            void updateField(
                const u32 budget, const u32 accumulation_samples = 8u, const float history_retention = 0.9f) {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not m_field.update(
                    *encoder, m_source, 1.0f / 60.0f, budget, accumulation_samples, history_retention));
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

        PPR_UNIT_TEST (field_accumulation_caps_count_and_retention_holds_history) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 0.5f});
            PPR_TEST_ASSERT(plan.has_value());
            // A budget covering the whole field makes every update re-trace the
            // nearby sample, so it reaches the accumulation cap and stays there.
            const u32 budget = static_cast<u32>(plan->m_radiance_samples);

            // One opaque emissive cell at (0,0); level-0 probe (0,0) direction 2 is near it.
            std::vector<u16> chunk(128u * 128u, 0u);
            chunk[0] = 1u;
            const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
            PPR_TEST_ASSERT(not source.applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 1u}));
            fixture.encodeSource();

            const u64 nearby_sample = 2u;
            const u64 nearby_word = nearby_sample * 4u;

            // accumulation = 2, retention = 0.5: the per-direction count caps at 2,
            // not 3 or 4, so the third sample uses the retention blend (not 1/3).
            for (u32 index = 0u; index < 4u; ++index) {
                fixture.updateField(budget, 2u, 0.5f);
            }
            const auto count = fixture.read<u32>(field.state());
            PPR_TEST_ASSERT(count[nearby_sample] == 2u);

            const auto radiance = fixture.read<u16>(field.view().m_radiance);
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[nearby_word + 0u]));
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[nearby_word + 1u]));
            PPR_TEST_ASSERT(halfIsFinitePositive(radiance[nearby_word + 2u]));

            // retention = 1.0: the steady-state EMA weight is zero, so one more
            // update leaves the radiance byte-identical (a plain running mean
            // would blend the new sample in and change it).
            fixture.updateField(budget, 2u, 1.0f);
            const auto count_held = fixture.read<u32>(field.state());
            PPR_TEST_ASSERT(count_held[nearby_sample] == 2u);
            const auto radiance_held = fixture.read<u16>(field.view().m_radiance);
            PPR_TEST_ASSERT(radiance_held == radiance);

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

        PPR_UNIT_TEST (field_routing_applies_dirty_chunk_and_binds_view) {
            constexpr u32 width = 256u;
            constexpr u32 height = 256u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &source = fixture.m_source;
            auto &field = fixture.m_field;

            const auto plan = lighting::planWorldLight({.m_width = width, .m_height = height, .m_cell_metres = 1.0f});
            PPR_TEST_ASSERT(plan.has_value());
            const u32 budget = static_cast<u32>(plan->m_levels[0].m_sample_count);

            // Committed stage: one dirty chunk (opaque emissive at cell (0,0))
            // plus one committed particle (palette index 0, the opaque emissive
            // material), exactly what the field-mode render lane consumes.
            GridStagedFrame stage{};
            stage.m_cell_size_m = 1.0f;
            stage.m_temporal_sky_hour = 7;
            GridStagedUpload upload{};
            upload.m_chunk_id = 0u;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            upload.m_cell_materials[0u] = 1u;
            stage.m_uploads.push_back(std::move(upload));
            auto particles = std::make_shared<Array<GridParticleInstance>>();
            particles->push_back({u64{1}, 2.5, 2.5, 0.0, 1.0, 300.0, 1u});
            stage.m_particle_batches.push_back(GridParticleBatch{0u, particles});

            // The producer maps the dirty chunk and the particle, then the source
            // adopts the revision (1).
            const auto storage = lighting::buildWorldLightSourceUpdate(stage, width, 1u, 7);
            PPR_TEST_ASSERT(storage.m_update.m_chunks.size() == 1u);
            PPR_TEST_ASSERT(storage.m_update.m_particles.size() == 1u);
            PPR_TEST_ASSERT(not source.applyUpdate(storage.m_update));
            PPR_TEST_ASSERT(source.revision() == 1u);

            // Encode the source upload and one field update, then read the fresh
            // ping-ponged view that the lane would bind to GridPass.
            fixture.encodeSource();
            fixture.updateField(budget);
            const auto view = field.view();
            PPR_TEST_ASSERT(view.m_radiance != nullptr);
            PPR_TEST_ASSERT(view.m_previous_radiance != nullptr);
            PPR_TEST_ASSERT(view.m_constants != nullptr);

            // The producer-built view binds to GridPass (the render-lane binding
            // step), and an empty view clears it back to the existing path.
            fixture.initializeGrid();
            PPR_TEST_ASSERT(not fixture.m_grid.setWorldLightField(view));
            PPR_TEST_ASSERT(not fixture.m_grid.setWorldLightField({}));

            // The cursor/cycle diagnostics remain readable as the field warms.
            fixture.updateField(budget);
            PPR_TEST_ASSERT(field.cursor() < plan->m_radiance_samples);

            // A second dirty edit advances the source revision again.
            stage.m_uploads.clear();
            stage.m_particle_batches.clear();
            GridStagedUpload second{};
            second.m_chunk_id = 0u;
            second.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            second.m_cell_materials[1u] = 1u;
            stage.m_uploads.push_back(std::move(second));
            const auto second_storage = lighting::buildWorldLightSourceUpdate(stage, width, 2u, 7);
            PPR_TEST_ASSERT(not source.applyUpdate(second_storage.m_update));
            PPR_TEST_ASSERT(source.revision() == 2u);

            fixture.shutdown();
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

            // Field cell metres match the staged chunk's cell_size_m (GridPass
            // admits only 1.0 or 1/3) so the world-space position conversion
            // aligns with the cascade field's cell grid.
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

            // A translucent optical-blend material must keep its own alpha/blend
            // path even while the field is enabled (never the opaque field body).
            GridTerrainVisual translucent = flat;
            translucent.m_material_id = 3u;
            translucent.m_surface_flags = kGridSurfaceOpticalBlend;
            translucent.m_opacity = 0.5f;
            stage.m_terrain_visuals.push_back(translucent);

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

            // One chunk with three regions: material 1 at (2,2), material 2 at
            // (6,2), and the translucent material 3 filling the 4x4 cell block
            // x in [2,6), y in [4,8) so its interior alpha dominates the AA rim.
            stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridStagedUpload upload{};
            upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            upload.m_cell_materials[2u * 128u + 2u] = 1u;
            upload.m_cell_materials[2u * 128u + 6u] = 2u;
            for (u32 y = 4u; y < 8u; ++y) {
                for (u32 x = 2u; x < 6u; ++x) {
                    upload.m_cell_materials[y * 128u + x] = 3u;
                }
            }
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
                    PPR_TEST_ASSERT(not grid.renderTranslucent(draw));
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

            // The translucent optical-blend block keeps its own alpha/blend path:
            // its interior coverage alpha is exactly the authored opacity (0.5),
            // so a large pixel count lands in a tight band around 0.5, never the
            // opaque field branch's forced 1.0 (the AA rim contributes far less).
            const auto translucent_pixels = std::ranges::count_if(
                lit, [](const std::array<float, 4u> &pixel) { return pixel[3] > 0.45f and pixel[3] < 0.55f; });
            PPR_TEST_ASSERT(translucent_pixels > 1000);

            // Release the test-owned target before the RHI teardown.
            target_view.setNull();
            target.setNull();

            fixture.shutdown();
        };

        PPR_UNIT_TEST (translucent_clipped_by_terrain) {
            constexpr u32 width = 64u;
            constexpr u32 height = 64u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &device = fixture.m_rhi->getDevice();
            fixture.initializeGrid();
            auto &grid = fixture.m_grid;

            // Orthographic camera viewing the [0,8)^2 cell region (16 px/cell).
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

            // Opaque blue cells, green gas particles, and two translucent red
            // cells (one absorbing, one flat) over a uniform white backdrop.
            GridStagedFrame stage{};
            stage.m_full_refresh = stage.m_reset_caches = stage.m_has_catalog = stage.m_has_background = true;
            stage.m_cell_size_m = 1.0f;
            stage.m_boundary_material_id = 1u;
            stage.m_content_fingerprint = 17u;

            GridTerrainVisual opaque{};
            opaque.m_material_id = 1u;
            opaque.m_color_rgba = 0x0000ffffu; // blue
            opaque.m_texture_index = 0u;
            opaque.m_transport_model = kGridTransportOpaqueV1;
            opaque.m_ior = 1.0f;
            opaque.m_roughness = 1.0f;
            stage.m_terrain_visuals.push_back(opaque);

            GridTerrainVisual gas = opaque;
            gas.m_material_id = 2u;
            gas.m_color_rgba = 0x00ff00ffu; // green
            stage.m_terrain_visuals.push_back(gas);

            GridTerrainVisual absorbing = opaque;
            absorbing.m_material_id = 3u;
            absorbing.m_color_rgba = 0xff0000ffu; // red
            absorbing.m_surface_flags = kGridSurfaceOpticalBlend;
            absorbing.m_transport_model = kGridTransportAbsorbingV1;
            absorbing.m_sigma_per_m[0] = 3.0f;
            absorbing.m_normal_texture_index = 1u;
            absorbing.m_opacity = 0.5f;
            absorbing.m_ior = 1.5f;
            absorbing.m_refraction_depth_m = 0.2f;
            stage.m_terrain_visuals.push_back(absorbing);

            GridTerrainVisual flat = absorbing;
            flat.m_material_id = 4u;
            flat.m_ior = 1.0f;
            flat.m_sigma_per_m[0] = 0.0f;
            stage.m_terrain_visuals.push_back(flat);

            stage.m_terrain_images.push_back(image({std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}}));
            stage.m_terrain_images.push_back(image({std::byte{191}, std::byte{128}, std::byte{255}, std::byte{255}}));

            for (u32 layer = 0u; layer < 4u; ++layer) {
                stage.m_background_layers[layer].m_texture_index = 0u;
                stage.m_background_layers[layer].m_order = layer;
                stage.m_background_layers[layer].m_color_rgba = 0xffffffffu;
            }

            stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridStagedUpload upload{};
            upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            upload.m_cell_materials[2u * 128u + 2u] = 1u; // opaque + clipped particle
            upload.m_cell_materials[4u * 128u + 2u] = 1u; // opaque control
            upload.m_cell_materials[2u * 128u + 4u] = 4u; // flat translucent + particle
            upload.m_cell_materials[4u * 128u + 4u] = 4u; // flat translucent control
            upload.m_cell_materials[2u * 128u + 6u] = 3u; // absorbing translucent
            upload.m_cell_materials[4u * 128u + 6u] = 4u; // flat translucent
            stage.m_uploads.push_back(std::move(upload));

            // Three green gas particles: one over the opaque cell (discarded), one
            // over a translucent cell (dimmed, not erased), one over vacuum
            // (blends). Radius 0.3 m with the reference 1 m thickness.
            const double volume = 0.3 * 0.3 * std::numbers::pi;
            auto particles = std::make_shared<Array<GridParticleInstance>>();
            particles->push_back({u64{1}, 2.5, 2.5, volume, 1.0, 300.0, 2u});
            particles->push_back({u64{2}, 6.5, 6.5, volume, 1.0, 300.0, 2u});
            particles->push_back({u64{3}, 4.5, 2.5, volume, 1.0, 300.0, 2u});
            stage.m_particle_batches.push_back(GridParticleBatch{0u, particles});

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
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(7u, 11u, 17u));

            PPR_TEST_ASSERT(not grid.update(
                TimeSpan{}, snapshot, false, GridPass::ReceiverContinuation::nearest, GridPass::ReceiverResponse::flat,
                GridPass::BackgroundResponse::unlit));

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

            const auto render = [&](const bool with_particles) {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                const PreRenderContext pre{device, *encoder, fixture.m_fence.completed(), {}};
                PPR_TEST_ASSERT(not grid.prepareBackground(pre, snapshot, viewport, scissor));
                PPR_TEST_ASSERT(not grid.prepareGpuVisibility(pre));
                if (with_particles) {
                    PPR_TEST_ASSERT(not grid.prepareGpuParticles(pre));
                }

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
                    const DrawContext draw{
                        device, *pass, RenderPipelineKey{signature}, viewport, scissor,
                        int2{int(target_edge), int(target_edge)}, fixture.m_fence.completed(), {}};
                    PPR_TEST_ASSERT(not grid.renderBackground(draw));
                    PPR_TEST_ASSERT(not grid.renderGpuIndirect(draw));
                    if (with_particles) {
                        PPR_TEST_ASSERT(not grid.renderParticles(draw));
                    }
                    PPR_TEST_ASSERT(not grid.renderTranslucent(draw));
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

            // Sample the centre of a cell (16 px/cell, world y = 0 at the bottom).
            const auto cell_centre = [](const int cx, const int cy) {
                return std::pair{8 + cx * 16, 8 + (7 - cy) * 16};
            };

            render(true);
            const auto with_particles = readback();
            render(false);
            const auto without_particles = readback();

            const auto [opaque_x, opaque_y] = cell_centre(2, 2);
            const auto [vacuum_x, vacuum_y] = cell_centre(6, 6);
            const auto [absorb_x, absorb_y] = cell_centre(6, 2);
            const auto [flat_x, flat_y] = cell_centre(6, 4);
            const auto [under_x, under_y] = cell_centre(4, 2);

            const auto at = [&](const std::vector<std::array<float, 4u>> &pixels, const int x, const int y) {
                return pixels[static_cast<std::size_t>(y) * target_edge + static_cast<std::size_t>(x)];
            };

            // Particle over opaque terrain is discarded: the opaque cell is
            // identical whether or not the particle pass runs, and stays
            // blue-dominant (the green particle never survives over it).
            const auto opaque_with = at(with_particles, opaque_x, opaque_y);
            const auto opaque_without = at(without_particles, opaque_x, opaque_y);
            PPR_TEST_ASSERT(std::abs(opaque_with[0] - opaque_without[0]) < 1e-3f);
            PPR_TEST_ASSERT(std::abs(opaque_with[1] - opaque_without[1]) < 1e-3f);
            PPR_TEST_ASSERT(std::abs(opaque_with[2] - opaque_without[2]) < 1e-3f);
            PPR_TEST_ASSERT(opaque_with[2] > opaque_with[1]);

            // Particle over vacuum blends: the green channel rises and dominates
            // the previously empty cell.
            const auto vacuum_with = at(with_particles, vacuum_x, vacuum_y);
            const auto vacuum_without = at(without_particles, vacuum_x, vacuum_y);
            const float vacuum_delta = std::max(
                {std::abs(vacuum_with[0] - vacuum_without[0]), std::abs(vacuum_with[1] - vacuum_without[1]),
                    std::abs(vacuum_with[2] - vacuum_without[2])});
            PPR_TEST_ASSERT(vacuum_delta > 0.05f);
            PPR_TEST_ASSERT(vacuum_with[1] > vacuum_with[0] and vacuum_with[1] > vacuum_with[2]);

            // Particle over a translucent cell is blended, not erased: the green
            // particle dims the translucent cell's transmitted backdrop (the red
            // channel drops) while the translucent surface still composites over
            // it. A full-replace (alpha 1.0) would leave both reads identical.
            const auto under_with = at(with_particles, under_x, under_y);
            const auto under_without = at(without_particles, under_x, under_y);
            PPR_TEST_ASSERT(under_without[0] - under_with[0] > 0.1f);

            // The absorbing translucent cell differs from a flat alpha blend of
            // the same albedo/opacity over the uniform backdrop: the red channel
            // is reduced by Beer-Lambert extinction while green/blue stay equal.
            const auto absorb = at(with_particles, absorb_x, absorb_y);
            const auto flat_px = at(with_particles, flat_x, flat_y);
            PPR_TEST_ASSERT(std::abs(absorb[0] - flat_px[0]) > 0.05f);
            PPR_TEST_ASSERT(std::abs(absorb[1] - flat_px[1]) < 0.05f);
            PPR_TEST_ASSERT(std::abs(absorb[2] - flat_px[2]) < 0.05f);

            target_view.setNull();
            target.setNull();

            fixture.shutdown();
        };

        PPR_UNIT_TEST (translucent_premultiplied_absorption) {
            constexpr u32 width = 64u;
            constexpr u32 height = 64u;

            Fixture fixture;
            fixture.initialize(width, height);
            auto &device = fixture.m_rhi->getDevice();
            fixture.initializeGrid();
            auto &grid = fixture.m_grid;

            // Orthographic camera viewing the [0,8)^2 cell region (16 px/cell).
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

            // Two flat (IOR 1.0, no normal map -> zero refraction offset) red
            // translucent cells: one absorbing, one not. The zero offset is what
            // exercised the old premultiplied early return that skipped the tint.
            GridStagedFrame stage{};
            stage.m_full_refresh = stage.m_reset_caches = stage.m_has_catalog = stage.m_has_background = true;
            stage.m_cell_size_m = 1.0f;
            stage.m_boundary_material_id = 1u;
            stage.m_content_fingerprint = 19u;

            GridTerrainVisual absorbing{};
            absorbing.m_material_id = 3u;
            absorbing.m_color_rgba = 0xff0000ffu; // red
            absorbing.m_texture_index = 0u;
            absorbing.m_transport_model = kGridTransportAbsorbingV1;
            absorbing.m_sigma_per_m[0] = 3.0f;
            absorbing.m_surface_flags = kGridSurfaceOpticalBlend;
            absorbing.m_opacity = 0.5f;
            absorbing.m_ior = 1.0f;
            absorbing.m_refraction_depth_m = 0.2f;
            absorbing.m_roughness = 1.0f;
            stage.m_terrain_visuals.push_back(absorbing);

            GridTerrainVisual flat = absorbing;
            flat.m_material_id = 4u;
            flat.m_sigma_per_m[0] = 0.0f;
            stage.m_terrain_visuals.push_back(flat);

            stage.m_terrain_images.push_back(image({std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}}));
            for (u32 layer = 0u; layer < 4u; ++layer) {
                stage.m_background_layers[layer].m_texture_index = 0u;
                stage.m_background_layers[layer].m_order = layer;
                stage.m_background_layers[layer].m_color_rgba = 0xffffffffu;
            }

            stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridStagedUpload upload{};
            upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            upload.m_cell_materials[2u * 128u + 2u] = 3u; // absorbing flat
            upload.m_cell_materials[2u * 128u + 6u] = 4u; // non-absorbing flat
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
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(7u, 11u, 19u));

            // Spatial MSAA selects the premultiplied fragment path.
            PPR_TEST_ASSERT(not grid.update(
                TimeSpan{}, snapshot, true, GridPass::ReceiverContinuation::nearest, GridPass::ReceiverResponse::flat,
                GridPass::BackgroundResponse::unlit));

            std::array<rhi::ComPtr<rhi::ITexture>, 2u> targets;
            std::array<rhi::ComPtr<rhi::ITextureView>, 2u> target_views;
            for (u32 index = 0u; index < targets.size(); ++index) {
                rhi::TextureDesc desc{};
                desc.type = index == 0u ? rhi::TextureType::Texture2D : rhi::TextureType::Texture2DMS;
                desc.sampleCount = index == 0u ? 1u : 4u;
                desc.size = {.width = target_edge, .height = target_edge, .depth = 1u};
                desc.format = rhi::Format::RGBA32Float;
                desc.memoryType = rhi::MemoryType::DeviceLocal;
                desc.usage = static_cast<rhi::TextureUsage>(
                    static_cast<u32>(rhi::TextureUsage::RenderTarget) |
                    static_cast<u32>(rhi::TextureUsage::CopySource));
                desc.defaultState = rhi::ResourceState::RenderTarget;
                desc.label = "translucent premultiplied target";
                PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, targets[index].writeRef())));
                PPR_TEST_ASSERT(not make_error_code(targets[index]->getDefaultView(target_views[index].writeRef())));
            }

            const auto viewport = rhi::Viewport::fromSize(float(target_edge), float(target_edge));
            const auto scissor = rhi::ScissorRect::fromSize(target_edge, target_edge);

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            const PreRenderContext pre{device, *encoder, fixture.m_fence.completed(), {}};
            PPR_TEST_ASSERT(not grid.prepareBackground(pre, snapshot, viewport, scissor));
            PPR_TEST_ASSERT(not grid.prepareGpuVisibility(pre));

            encoder->setTextureState(
                targets[1], rhi::SubresourceRange{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u},
                rhi::ResourceState::RenderTarget);
            rhi::RenderPassColorAttachment attachment{};
            attachment.view = target_views[1];
            attachment.resolveTarget = target_views[0];
            attachment.clearValue[0] = attachment.clearValue[1] = attachment.clearValue[2] = attachment.clearValue[3] =
                0.0f;
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
                signature.m_sample_count = SampleCount::x4;
                const DrawContext draw{
                    device, *pass, RenderPipelineKey{signature}, viewport, scissor,
                    int2{int(target_edge), int(target_edge)}, fixture.m_fence.completed(), {}};
                PPR_TEST_ASSERT(not grid.renderBackground(draw));
                PPR_TEST_ASSERT(not grid.renderGpuIndirect(draw));
                PPR_TEST_ASSERT(not grid.renderTranslucent(draw));
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

            shader::ComPtr<ISlangBlob> blob;
            rhi::SubresourceLayout layout{};
            PPR_TEST_ASSERT(not make_error_code(device.readTexture(targets[0], 0u, 0u, blob.writeRef(), &layout)));
            const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());
            std::vector<std::array<float, 4u>> pixels(std::size_t(target_edge) * target_edge);
            for (u32 y = 0u; y < target_edge; ++y) {
                for (u32 x = 0u; x < target_edge; ++x) {
                    std::array<float, 4u> pixel{};
                    std::memcpy(pixel.data(), source + y * layout.rowPitch + x * sizeof(pixel), sizeof(pixel));
                    pixels[y * target_edge + x] = pixel;
                }
            }

            const auto at = [&](const int cx, const int cy) {
                const int x = 8 + cx * 16;
                const int y = 8 + (7 - cy) * 16;
                return pixels[static_cast<std::size_t>(y) * target_edge + static_cast<std::size_t>(x)];
            };

            // The absorbing flat cell's red channel is reduced by Beer-Lambert
            // extinction even on the premultiplied (spatial MSAA) path, while
            // green/blue stay equal to the non-absorbing flat cell.
            const auto absorb = at(2, 2);
            const auto flat_px = at(6, 2);
            PPR_TEST_ASSERT(std::abs(absorb[0] - flat_px[0]) > 0.05f);
            PPR_TEST_ASSERT(std::abs(absorb[1] - flat_px[1]) < 0.05f);
            PPR_TEST_ASSERT(std::abs(absorb[2] - flat_px[2]) < 0.05f);

            // Release the device-owned command/readback objects before the RHI
            // service tears the device down, or destroyRHI() asserts a live device.
            blob.setNull();
            commands.setNull();
            encoder.setNull();

            for (auto &view: target_views) {
                view.setNull();
            }
            for (auto &target: targets) {
                target.setNull();
            }

            fixture.shutdown();
        };
    }

    // ------------------------------------------------------------------
    // histogram eye adaptation
    // ------------------------------------------------------------------

    namespace HistogramAdaptation {
        PPR_UNIT_TEST (cpu_histogram_math) {
            constexpr float ev_min = -4.0f;
            constexpr float ev_max = 4.0f;

            // Black frame: every luminance is zero. The log2 clamp keeps the
            // reduction finite and the target EV pins to the lower bound.
            std::vector<float> black(4096u, 0.0f);
            std::array<u32, RetroPass::kHistogramBins> black_hist{};
            for (const float lum: black) {
                ++black_hist[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
            }
            const float black_avg = RetroPass::averageLog2Luminance(black_hist, static_cast<u32>(black.size()));
            PPR_TEST_ASSERT(std::isfinite(black_avg));
            PPR_TEST_ASSERT(RetroPass::targetEv(black_avg, ev_min, ev_max) == ev_min);

            // Uniform 0.18 middle gray: target EV lands near zero.
            std::vector<float> uniform(4096u, 0.18f);
            std::array<u32, RetroPass::kHistogramBins> uniform_hist{};
            for (const float lum: uniform) {
                ++uniform_hist[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
            }
            const float uniform_avg = RetroPass::averageLog2Luminance(uniform_hist, static_cast<u32>(uniform.size()));
            const float uniform_ev = RetroPass::targetEv(uniform_avg, ev_min, ev_max);
            PPR_TEST_ASSERT(std::isfinite(uniform_ev));
            PPR_TEST_ASSERT(std::abs(uniform_ev) < 0.25f);

            // Bright-emitter dominated: one emitter among a dark background. The
            // percentile clip suppresses the emitter, so the target stays below
            // the middle-gray target and within the clamp window.
            std::vector<float> emitter(4096u, 0.01f);
            emitter[0] = 1000.0f;
            std::array<u32, RetroPass::kHistogramBins> emitter_hist{};
            for (const float lum: emitter) {
                ++emitter_hist[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
            }
            const float emitter_avg = RetroPass::averageLog2Luminance(emitter_hist, static_cast<u32>(emitter.size()));
            const float emitter_ev = RetroPass::targetEv(emitter_avg, ev_min, ev_max);
            PPR_TEST_ASSERT(std::isfinite(emitter_ev));
            PPR_TEST_ASSERT(emitter_ev < uniform_ev);
            PPR_TEST_ASSERT(emitter_ev >= ev_min);
        };

        PPR_UNIT_TEST (cpu_adaptation_alpha) {
            // A 60 Hz frame blends a small fraction, never an instant snap.
            const float slow = RetroPass::adaptationAlpha(std::chrono::milliseconds{16}, 0.7f);
            PPR_TEST_ASSERT(slow > 0.0f and slow < 0.1f);

            // A cumulative timestamp (e.g. an hour since startup) would drive the
            // blend to ~1; the adaptation path must never treat request_elapsed as
            // the frame delta, or this is what it would compute.
            const float timestamp = RetroPass::adaptationAlpha(std::chrono::hours{1}, 0.7f);
            PPR_TEST_ASSERT(timestamp > 0.99f);

            // Zero delta and non-positive tau never adapt.
            PPR_TEST_ASSERT(RetroPass::adaptationAlpha(TimeSpan{}, 0.7f) == 0.0f);
            PPR_TEST_ASSERT(RetroPass::adaptationAlpha(std::chrono::milliseconds{16}, 0.0f) == 0.0f);
        };

        // GPU smoke drives the real luminance_histogram.slang + auto_exposure.slang
        // compute entries against a synthetic uniform HDR scene, then reads the
        // adapted offset back once (no per-frame readback in production).
        namespace Gpu {
            struct Fixture {
                safe_ptr<IShaderService> m_shaders{IShaderService::get()};
                safe_ptr<IRhiService> m_rhi{IRhiService::get()};
                rhi::ComPtr<rhi::ICommandQueue> m_queue;
                FrameFence m_fence;

                rhi::ComPtr<rhi::ITexture> m_scene;
                rhi::ComPtr<rhi::ITextureView> m_scene_view;
                rhi::ComPtr<rhi::ITexture> m_luminance;
                rhi::ComPtr<rhi::ITextureView> m_luminance_view;
                rhi::ComPtr<rhi::IBuffer> m_histogram;
                rhi::ComPtr<rhi::IBuffer> m_target;
                std::array<rhi::ComPtr<rhi::ITexture>, 2> m_exposure;
                std::array<rhi::ComPtr<rhi::ITextureView>, 2> m_exposure_view;
                std::array<rhi::ComPtr<rhi::IShaderProgram>, 4> m_histogram_programs;
                std::array<rhi::ComPtr<rhi::IComputePipeline>, 4> m_histogram_pipelines;
                std::array<rhi::ComPtr<rhi::IShaderObject>, 4> m_histogram_roots;
                rhi::ComPtr<rhi::IShaderProgram> m_auto_program;
                rhi::ComPtr<rhi::IComputePipeline> m_auto_pipeline;
                rhi::ComPtr<rhi::IShaderObject> m_auto_root;
                u32 m_exposure_index{};

                [[nodiscard]] static rhi::BufferRange range_(rhi::IBuffer *const buffer) noexcept {
                    return {.offset = 0u, .size = buffer->getDesc().size};
                }

                void initialize(const float luminance) {
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

                    // Synthetic RGBA32Float scene, uniform luminance, uploaded once.
                    const u32 edge = 64u;
                    std::vector<float> scene(std::size_t(edge) * edge * 4u);
                    for (std::size_t pixel = 0u; pixel < std::size_t(edge) * edge; ++pixel) {
                        scene[pixel * 4u + 0u] = luminance;
                        scene[pixel * 4u + 1u] = luminance;
                        scene[pixel * 4u + 2u] = luminance;
                        scene[pixel * 4u + 3u] = 1.0f;
                    }
                    const rhi::SubresourceData initial{
                        .data = scene.data(),
                        .rowPitch = edge * sizeof(float) * 4u,
                        .slicePitch = std::size_t(edge) * edge * sizeof(float) * 4u};
                    rhi::TextureDesc scene_desc{};
                    scene_desc.type = rhi::TextureType::Texture2D;
                    scene_desc.size = {.width = edge, .height = edge, .depth = 1u};
                    scene_desc.arrayLength = 1u;
                    scene_desc.mipCount = 1u;
                    scene_desc.format = rhi::Format::RGBA32Float;
                    scene_desc.usage = rhi::TextureUsage::ShaderResource;
                    scene_desc.defaultState = rhi::ResourceState::ShaderResource;
                    scene_desc.label = "histogram synthetic scene";
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createTexture(scene_desc, &initial, m_scene.writeRef())));
                    PPR_TEST_ASSERT(not make_error_code(m_scene->getDefaultView(m_scene_view.writeRef())));

                    const auto uav_texture = [&](const u32 width, const u32 height, const char *const label,
                                                 rhi::ComPtr<rhi::ITexture> &out,
                                                 rhi::ComPtr<rhi::ITextureView> &view) {
                        rhi::TextureDesc desc{};
                        desc.type = rhi::TextureType::Texture2D;
                        desc.size = {.width = width, .height = height, .depth = 1u};
                        desc.arrayLength = 1u;
                        desc.mipCount = 1u;
                        desc.format = rhi::Format::R32Float;
                        desc.usage = static_cast<rhi::TextureUsage>(
                            static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                            static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
                        desc.defaultState = rhi::ResourceState::UnorderedAccess;
                        desc.label = label;
                        PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, out.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(out->getDefaultView(view.writeRef())));
                    };
                    uav_texture(edge, edge, "histogram luminance", m_luminance, m_luminance_view);

                    // Zero-initialized exposure so the first adaptation frame reads
                    // a finite previous offset (0) rather than uninitialized memory.
                    const float zero = 0.0f;
                    const rhi::SubresourceData zero_initial{
                        .data = &zero, .rowPitch = sizeof(float), .slicePitch = sizeof(float)};
                    const auto exposure_texture = [&](const char *const label, rhi::ComPtr<rhi::ITexture> &out,
                                                      rhi::ComPtr<rhi::ITextureView> &view) {
                        rhi::TextureDesc desc{};
                        desc.type = rhi::TextureType::Texture2D;
                        desc.size = {.width = 1u, .height = 1u, .depth = 1u};
                        desc.arrayLength = 1u;
                        desc.mipCount = 1u;
                        desc.format = rhi::Format::R32Float;
                        desc.usage = static_cast<rhi::TextureUsage>(
                            static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                            static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
                        desc.defaultState = rhi::ResourceState::UnorderedAccess;
                        desc.label = label;
                        PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, &zero_initial, out.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(out->getDefaultView(view.writeRef())));
                    };
                    exposure_texture("adaptation exposure 0", m_exposure[0], m_exposure_view[0]);
                    exposure_texture("adaptation exposure 1", m_exposure[1], m_exposure_view[1]);

                    const auto device_buffer = [&](const u64 bytes, const char *const label,
                                                   rhi::ComPtr<rhi::IBuffer> &out) {
                        rhi::BufferDesc desc{};
                        desc.size = bytes;
                        desc.elementSize = sizeof(u32);
                        desc.memoryType = rhi::MemoryType::DeviceLocal;
                        desc.usage = static_cast<rhi::BufferUsage>(
                            static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                            static_cast<u32>(rhi::BufferUsage::CopySource));
                        desc.defaultState = rhi::ResourceState::UnorderedAccess;
                        desc.label = label;
                        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, nullptr, out.writeRef())));
                    };
                    device_buffer(RetroPass::kHistogramBins * sizeof(u32), "histogram bins", m_histogram);
                    device_buffer(sizeof(float), "histogram target EV", m_target);

                    shader::SharedModule histogram_module{};
                    PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                        std::filesystem::current_path() / "shaders" / "luminance_histogram.slang",
                        "luminance_histogram", histogram_module.writeRef()));
                    const char *const histogram_entries[] = {
                        "clearHistogram", "computeLuminance", "buildHistogram", "reduceHistogram"};
                    for (u32 index = 0u; index < 4u; ++index) {
                        shader::ComPtr<slang::IEntryPoint> entry;
                        PPR_TEST_ASSERT(not make_error_code(
                            histogram_module->findEntryPointByName(histogram_entries[index], entry.writeRef())));
                        slang::IComponentType *entries[] = {entry.get()};
                        rhi::ShaderProgramDesc desc{};
                        desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                        desc.slangGlobalScope = histogram_module.get();
                        desc.slangEntryPoints = entries;
                        desc.slangEntryPointCount = 1u;
                        shader::Diagnose diagnostics;
                        PPR_TEST_ASSERT(not make_error_code(device.createShaderProgram(
                            desc, m_histogram_programs[index].writeRef(), diagnostics.writeRef())));
                        rhi::ComputePipelineDesc pipeline_desc{};
                        pipeline_desc.program = m_histogram_programs[index];
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createComputePipeline(pipeline_desc, m_histogram_pipelines[index].writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(
                            m_histogram_programs[index].get(), m_histogram_roots[index].writeRef())));
                    }

                    shader::SharedModule auto_module{};
                    PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                        std::filesystem::current_path() / "shaders" / "auto_exposure.slang", "auto_exposure",
                        auto_module.writeRef()));
                    shader::ComPtr<slang::IEntryPoint> auto_entry;
                    PPR_TEST_ASSERT(
                        not make_error_code(auto_module->findEntryPointByName("adaptExposure", auto_entry.writeRef())));
                    slang::IComponentType *auto_entries[] = {auto_entry.get()};
                    rhi::ShaderProgramDesc auto_desc{};
                    auto_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                    auto_desc.slangGlobalScope = auto_module.get();
                    auto_desc.slangEntryPoints = auto_entries;
                    auto_desc.slangEntryPointCount = 1u;
                    shader::Diagnose auto_diagnostics;
                    PPR_TEST_ASSERT(not make_error_code(
                        device.createShaderProgram(auto_desc, m_auto_program.writeRef(), auto_diagnostics.writeRef())));
                    rhi::ComputePipelineDesc auto_pipeline_desc{};
                    auto_pipeline_desc.program = m_auto_program;
                    PPR_TEST_ASSERT(not make_error_code(
                        device.createComputePipeline(auto_pipeline_desc, m_auto_pipeline.writeRef())));
                    PPR_TEST_ASSERT(not make_error_code(
                        device.createRootShaderObject(m_auto_program.get(), m_auto_root.writeRef())));
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
                std::vector<T> readBuffer(rhi::IBuffer *const source) {
                    rhi::BufferDesc description{};
                    description.size = source->getDesc().size;
                    description.elementSize = 0u;
                    description.memoryType = rhi::MemoryType::ReadBack;
                    description.usage = rhi::BufferUsage::CopyDestination;
                    description.defaultState = rhi::ResourceState::CopyDestination;
                    description.label = "histogram adaptation readback";
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

                // Runs clear -> luminance downsample -> histogram build -> reduce
                // and returns the reduced target EV via fenced buffer readback.
                float reduceTarget() {
                    struct HistogramParameters {
                        u32 m_source_extent[2];
                        u32 m_luminance_extent[2];
                        u32 m_reserved0;
                        u32 m_reserved1;
                    };

                    const u32 edge = 64u;
                    const HistogramParameters histogram{{edge, edge}, {edge, edge}, 0u, 0u};

                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));

                    const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                    for (u32 index = 0u; index < 4u; ++index) {
                        rhi::ShaderCursor cursor{m_histogram_roots[index]}, constants{};
                        PPR_TEST_ASSERT(not make_error_code(cursor["g_luminance"].getDereferenced(constants)));
                        PPR_TEST_ASSERT(not make_error_code(constants.setData(&histogram, sizeof(histogram))));
                        PPR_TEST_ASSERT(not make_error_code(cursor["g_scene"].setBinding(rhi::Binding(m_scene_view))));
                        PPR_TEST_ASSERT(not make_error_code(
                            cursor["g_luminance_image"].setBinding(rhi::Binding(m_luminance_view))));
                        PPR_TEST_ASSERT(not make_error_code(
                            cursor["g_histogram"].setBinding(rhi::Binding(m_histogram, range_(m_histogram)))));
                        PPR_TEST_ASSERT(not make_error_code(
                            cursor["g_target_ev"].setBinding(rhi::Binding(m_target, range_(m_target)))));
                    }

                    encoder->setTextureState(m_luminance, range, rhi::ResourceState::UnorderedAccess);
                    encoder->setBufferState(m_histogram, rhi::ResourceState::UnorderedAccess);
                    encoder->setBufferState(m_target, rhi::ResourceState::UnorderedAccess);

                    const auto dispatch = [&](const u32 index, const u32 x, const u32 y) {
                        auto *const pass = encoder->beginComputePass();
                        PPR_TEST_ASSERT(pass != nullptr);
                        pass->bindPipeline(m_histogram_pipelines[index], m_histogram_roots[index]);
                        pass->dispatchCompute(x, y, 1u);
                        pass->end();
                    };

                    dispatch(0u, 1u, 1u); // clearHistogram
                    encoder->globalBarrier();
                    dispatch(1u, edge / 8u, edge / 8u); // computeLuminance
                    encoder->globalBarrier();
                    dispatch(2u, edge / 8u, edge / 8u); // buildHistogram
                    encoder->globalBarrier();
                    dispatch(3u, 1u, 1u); // reduceHistogram
                    submit(*encoder);

                    const auto out = readBuffer<float>(m_target);
                    PPR_TEST_ASSERT(out.size() == 1u);
                    return out[0];
                }

                // Runs one adaptation frame (the histogram target must already be
                // reduced) and returns the newly written adapted offset.
                float adapt(const TimeSpan dt, const float tau, const bool enabled) {
                    struct AutoParameters {
                        float m_alpha;
                        float m_ev_min;
                        float m_ev_max;
                        u32 m_enabled;
                    };

                    const AutoParameters automatic{RetroPass::adaptationAlpha(dt, tau), -4.0f, 4.0f, enabled ? 1u : 0u};

                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));

                    const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                    const u32 previous = m_exposure_index;
                    const u32 current = 1u - m_exposure_index;
                    m_exposure_index = current;

                    rhi::ShaderCursor cursor{m_auto_root}, constants{};
                    PPR_TEST_ASSERT(not make_error_code(cursor["g_auto_exposure"].getDereferenced(constants)));
                    PPR_TEST_ASSERT(not make_error_code(constants.setData(&automatic, sizeof(automatic))));
                    PPR_TEST_ASSERT(not make_error_code(
                        cursor["g_target_ev"].setBinding(rhi::Binding(m_target, range_(m_target)))));
                    PPR_TEST_ASSERT(not make_error_code(
                        cursor["g_previous_exposure"].setBinding(rhi::Binding(m_exposure_view[previous]))));
                    PPR_TEST_ASSERT(
                        not make_error_code(cursor["g_exposure"].setBinding(rhi::Binding(m_exposure_view[current]))));

                    encoder->setTextureState(m_exposure[current], range, rhi::ResourceState::UnorderedAccess);
                    auto *const pass = encoder->beginComputePass();
                    PPR_TEST_ASSERT(pass != nullptr);
                    pass->bindPipeline(m_auto_pipeline, m_auto_root);
                    pass->dispatchCompute(1u, 1u, 1u);
                    pass->end();
                    encoder->setTextureState(m_exposure[current], range, rhi::ResourceState::ShaderResource);

                    submit(*encoder);

                    shader::ComPtr<ISlangBlob> blob;
                    rhi::SubresourceLayout layout{};
                    PPR_TEST_ASSERT(not make_error_code(
                        m_rhi->getDevice().readTexture(m_exposure[current], 0u, 0u, blob.writeRef(), &layout)));
                    const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());
                    float value{};
                    std::memcpy(&value, source, sizeof(value));
                    return value;
                }

                void shutdown() {
                    if (m_queue and make_error_code(m_queue->waitOnHost()))
                        hal::fatalExit("histogram-adaptation-test-quiescence-unknown");

                    m_auto_root.setNull();
                    m_auto_pipeline.setNull();
                    m_auto_program.setNull();
                    m_histogram_roots = {};
                    m_histogram_pipelines = {};
                    m_histogram_programs = {};
                    m_target.setNull();
                    m_histogram.setNull();
                    m_exposure_view[0].setNull();
                    m_exposure_view[1].setNull();
                    m_exposure[0].setNull();
                    m_exposure[1].setNull();
                    m_luminance_view.setNull();
                    m_luminance.setNull();
                    m_scene_view.setNull();
                    m_scene.setNull();
                    PPR_TEST_ASSERT(not m_fence.shutdown());
                    m_queue.setNull();
                    PPR_TEST_ASSERT(not m_rhi->shutdown());
                    PPR_TEST_ASSERT(not m_shaders->shutdown());
                }
            };

            PPR_UNIT_TEST (gpu_adaptation_smoke) {
                constexpr float ev_min = -4.0f;
                constexpr float ev_max = 4.0f;
                const TimeSpan dt = std::chrono::milliseconds{16}; // ~60 Hz frame
                const float tau = 0.7f;
                const float alpha = RetroPass::adaptationAlpha(dt, tau);

                Fixture fixture;
                fixture.initialize(0.18f);
                PPR_DEFER {
                    fixture.shutdown();
                };

                // CPU oracle target for the uniform 0.18 scene (64x64 luminance).
                // The GPU reduction writes the unclamped target EV; the final clamp
                // happens in adaptExposure, so compare against the unclamped value.
                std::vector<float> uniform(std::size_t(64u) * 64u, 0.18f);
                std::array<u32, RetroPass::kHistogramBins> histogram{};
                for (const float lum: uniform) {
                    ++histogram[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
                }
                const float cpu_target = RetroPass::averageLog2Luminance(histogram, static_cast<u32>(uniform.size())) -
                                         std::log2(RetroPass::kMiddleGrayLuminance);

                // GPU reduced target must match the CPU oracle (numeric, not just
                // finiteness): a broken histogram/reduction fails here.
                const float gpu_target = fixture.reduceTarget();
                PPR_TEST_ASSERT(std::isfinite(gpu_target));
                PPR_TEST_ASSERT(std::abs(gpu_target - cpu_target) < 0.05f);

                // A 60 Hz frame adapts slowly toward the target: the first frame is
                // target*alpha, not an instant snap to the target.
                const float first = fixture.adapt(dt, tau, true);
                PPR_TEST_ASSERT(std::isfinite(first));
                PPR_TEST_ASSERT(std::abs(first - cpu_target * alpha) < 0.05f);
                PPR_TEST_ASSERT(std::abs(first) < std::abs(cpu_target) * 0.5f);

                // A second identical frame moves a little further, still bounded.
                const float second = fixture.adapt(dt, tau, true);
                PPR_TEST_ASSERT(std::isfinite(second) and second >= ev_min and second <= ev_max);
                PPR_TEST_ASSERT(std::abs(second - first) < 0.05f);

                // Fixed exposure (disabled) writes the zero offset deterministically.
                const float fixed_a = fixture.adapt(dt, tau, false);
                const float fixed_b = fixture.adapt(dt, tau, false);
                PPR_TEST_ASSERT(fixed_a == 0.0f);
                PPR_TEST_ASSERT(fixed_b == 0.0f);
            };
        }

        const UnitTest histogram_adaptation = UnitTest::Named("histogram_adaptation") /
                                              [](UnitTest::IRun &run) -> void {
            run.recurse({cpu_histogram_math, cpu_adaptation_alpha, Gpu::gpu_adaptation_smoke});
        };
    }

    // ------------------------------------------------------------------
    // SPD-style bloom
    // ------------------------------------------------------------------

    namespace SpdBloom {
        PPR_UNIT_TEST (cpu_bloom_chain_layout) {
            // A 1080p picture: half-res mip 0, six mips, floor-halved extents.
            const int2 mip0 = RetroPass::bloomMip0Extent(int2{1920, 1080});
            PPR_TEST_ASSERT(mip0.x == 960 and mip0.y == 540);
            PPR_TEST_ASSERT(RetroPass::bloomMipCount(mip0) == 6u);
            PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 0u).x == 960);
            PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 1u).x == 480);
            PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 5u).x == 30);
            PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 5u).y == 16);

            // Cumulative RGBA16F bytes (8 per texel) over the six-mip chain.
            const u64 expected = (960ull * 540 + 480ull * 270 + 240ull * 135 + 120ull * 67 + 60ull * 33 + 30ull * 16) *
                                 8u;
            PPR_TEST_ASSERT(RetroPass::bloomChainBytes(mip0) == expected);

            // Odd extents stay floor-halved and clamped at one texel.
            const int2 odd = RetroPass::bloomMip0Extent(int2{101, 51});
            PPR_TEST_ASSERT(odd.x == 51 and odd.y == 26);
            PPR_TEST_ASSERT(RetroPass::bloomMipExtent(odd, 0u).y == 26);
            PPR_TEST_ASSERT(RetroPass::bloomMipExtent(odd, 5u).x == 1);
            PPR_TEST_ASSERT(RetroPass::bloomMipExtent(odd, 31u).y == 1);

            // A tiny chain stays a single mip (still capped at six, never zero).
            PPR_TEST_ASSERT(RetroPass::bloomMipCount(int2{1, 1}) == 1u);
        };

        namespace Gpu {
            struct BloomParams {
                u32 m_source[2];
                u32 m_mip0[2];
                float m_threshold;
                float m_soft_knee;
                u32 m_mip_count;
                u32 m_upsample_level;
            };
            static_assert(sizeof(BloomParams) == 32u);

            struct Fixture {
                safe_ptr<IShaderService> m_shaders{IShaderService::get()};
                safe_ptr<IRhiService> m_rhi{IRhiService::get()};
                rhi::ComPtr<rhi::ICommandQueue> m_queue;
                FrameFence m_fence;

                rhi::ComPtr<rhi::ITexture> m_bloom;
                rhi::ComPtr<rhi::ITextureView> m_bloom_view;
                std::array<rhi::ComPtr<rhi::ITextureView>, RetroPass::kMaxBloomMips> m_bloom_uav_views{};
                std::array<rhi::ComPtr<rhi::IShaderProgram>, 3> m_programs{};
                std::array<rhi::ComPtr<rhi::IComputePipeline>, 3> m_pipelines{};
                std::array<rhi::ComPtr<rhi::IShaderObject>, 3> m_roots{};

                void initialize(const u32 edge) {
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

                    const int2 mip0 = RetroPass::bloomMip0Extent(int2{int(edge), int(edge)});
                    rhi::TextureDesc bloom_desc{};
                    bloom_desc.type = rhi::TextureType::Texture2D;
                    bloom_desc.size = {.width = u32(mip0.x), .height = u32(mip0.y), .depth = 1u};
                    bloom_desc.arrayLength = 1u;
                    bloom_desc.mipCount = RetroPass::kMaxBloomMips;
                    bloom_desc.format = rhi::Format::RGBA16Float;
                    bloom_desc.usage = static_cast<rhi::TextureUsage>(
                        static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                        static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
                    bloom_desc.defaultState = rhi::ResourceState::UnorderedAccess;
                    bloom_desc.label = "spd bloom chain";
                    PPR_TEST_ASSERT(not make_error_code(device.createTexture(bloom_desc, nullptr, m_bloom.writeRef())));
                    PPR_TEST_ASSERT(not make_error_code(m_bloom->getDefaultView(m_bloom_view.writeRef())));
                    for (u32 level = 0u; level < RetroPass::kMaxBloomMips; ++level) {
                        rhi::TextureViewDesc view_desc{};
                        view_desc.subresourceRange = {.layer = 0u, .layerCount = 1u, .mip = level, .mipCount = 1u};
                        PPR_TEST_ASSERT(
                            not make_error_code(m_bloom->createView(view_desc, m_bloom_uav_views[level].writeRef())));
                    }

                    shader::SharedModule module{};
                    PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                        std::filesystem::current_path() / "shaders" / "spd_bloom.slang", "spd_bloom",
                        module.writeRef()));
                    const char *const entries[] = {"prefilterBloom", "downsampleBloom", "upsampleBloom"};
                    for (u32 index = 0u; index < 3u; ++index) {
                        shader::ComPtr<slang::IEntryPoint> entry;
                        PPR_TEST_ASSERT(
                            not make_error_code(module->findEntryPointByName(entries[index], entry.writeRef())));
                        slang::IComponentType *component_entries[] = {entry.get()};
                        rhi::ShaderProgramDesc desc{};
                        desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                        desc.slangGlobalScope = module.get();
                        desc.slangEntryPoints = component_entries;
                        desc.slangEntryPointCount = 1u;
                        shader::Diagnose diagnostics;
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createShaderProgram(desc, m_programs[index].writeRef(), diagnostics.writeRef())));
                        rhi::ComputePipelineDesc pipeline_desc{};
                        pipeline_desc.program = m_programs[index];
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createComputePipeline(pipeline_desc, m_pipelines[index].writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createRootShaderObject(m_programs[index].get(), m_roots[index].writeRef())));
                    }
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

                // Uploads a synthetic scene (uniform background plus one bright
                // block), runs the full prefilter/downsample/upsample chain, and
                // returns the accumulated mip 0 decoded to linear float RGB.
                std::vector<std::array<float, 3>> run(const u32 edge, const float background, const float block_value) {
                    const int2 mip0 = RetroPass::bloomMip0Extent(int2{int(edge), int(edge)});
                    const u32 mip_count = RetroPass::bloomMipCount(mip0);
                    const u32 block_edge = edge / 16u;
                    const u32 block_origin = (edge - block_edge) / 2u;

                    std::vector<float> scene(std::size_t(edge) * edge * 4u);
                    for (u32 y = 0u; y < edge; ++y) {
                        for (u32 x = 0u; x < edge; ++x) {
                            const bool in_block = x >= block_origin and
                                                  x < block_origin + block_edge and
                                                  y >= block_origin and
                                                  y < block_origin + block_edge;
                            const float value = in_block ? block_value : background;
                            scene[(std::size_t(y) * edge + x) * 4u + 0u] = value;
                            scene[(std::size_t(y) * edge + x) * 4u + 1u] = value;
                            scene[(std::size_t(y) * edge + x) * 4u + 2u] = value;
                            scene[(std::size_t(y) * edge + x) * 4u + 3u] = 1.0f;
                        }
                    }
                    const rhi::SubresourceData initial{
                        .data = scene.data(),
                        .rowPitch = edge * sizeof(float) * 4u,
                        .slicePitch = std::size_t(edge) * edge * sizeof(float) * 4u};
                    rhi::TextureDesc scene_desc{};
                    scene_desc.type = rhi::TextureType::Texture2D;
                    scene_desc.size = {.width = edge, .height = edge, .depth = 1u};
                    scene_desc.arrayLength = 1u;
                    scene_desc.mipCount = 1u;
                    scene_desc.format = rhi::Format::RGBA32Float;
                    scene_desc.usage = rhi::TextureUsage::ShaderResource;
                    scene_desc.defaultState = rhi::ResourceState::ShaderResource;
                    scene_desc.label = "spd bloom synthetic scene";
                    rhi::ComPtr<rhi::ITexture> scene_tex;
                    rhi::ComPtr<rhi::ITextureView> scene_view;
                    auto &device = m_rhi->getDevice();
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createTexture(scene_desc, &initial, scene_tex.writeRef())));
                    PPR_TEST_ASSERT(not make_error_code(scene_tex->getDefaultView(scene_view.writeRef())));

                    BloomParams params{};
                    params.m_source[0] = edge;
                    params.m_source[1] = edge;
                    params.m_mip0[0] = u32(mip0.x);
                    params.m_mip0[1] = u32(mip0.y);
                    params.m_threshold = 1.0f;
                    params.m_soft_knee = RetroPass::kBloomSoftKnee;
                    params.m_mip_count = mip_count;
                    params.m_upsample_level = 0u;

                    static constexpr std::array<const char *, RetroPass::kMaxBloomMips> kMipNames{
                        "g_bloom_mip0", "g_bloom_mip1", "g_bloom_mip2", "g_bloom_mip3", "g_bloom_mip4", "g_bloom_mip5"};

                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));

                    {
                        rhi::ShaderCursor cursor{m_roots[0]}, constants{};
                        PPR_TEST_ASSERT(not make_error_code(cursor["g_bloom"].getDereferenced(constants)));
                        PPR_TEST_ASSERT(not make_error_code(constants.setData(&params, sizeof(params))));
                        PPR_TEST_ASSERT(not make_error_code(cursor["g_scene"].setBinding(rhi::Binding(scene_view))));
                        PPR_TEST_ASSERT(
                            not make_error_code(cursor["g_bloom_mip0"].setBinding(rhi::Binding(m_bloom_uav_views[0]))));
                    }
                    {
                        rhi::ShaderCursor cursor{m_roots[1]}, constants{};
                        PPR_TEST_ASSERT(not make_error_code(cursor["g_bloom"].getDereferenced(constants)));
                        PPR_TEST_ASSERT(not make_error_code(constants.setData(&params, sizeof(params))));
                        for (u32 level = 0u; level < RetroPass::kMaxBloomMips; ++level) {
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor[kMipNames[level]].setBinding(rhi::Binding(m_bloom_uav_views[level]))));
                        }
                    }
                    rhi::ShaderCursor up_cursor{m_roots[2]}, up_constants{};
                    PPR_TEST_ASSERT(not make_error_code(up_cursor["g_bloom"].getDereferenced(up_constants)));

                    const rhi::SubresourceRange chain{
                        .layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = RetroPass::kMaxBloomMips};
                    encoder->setTextureState(m_bloom, chain, rhi::ResourceState::UnorderedAccess);

                    const auto dispatch = [&](const u32 program, const u32 x, const u32 y) {
                        auto *const pass = encoder->beginComputePass();
                        PPR_TEST_ASSERT(pass != nullptr);
                        pass->bindPipeline(m_pipelines[program], m_roots[program]);
                        pass->dispatchCompute(x, y, 1u);
                        pass->end();
                    };
                    dispatch(0u, (u32(mip0.x) + 7u) / 8u, (u32(mip0.y) + 7u) / 8u);
                    encoder->globalBarrier();
                    dispatch(1u, (u32(mip0.x) + 31u) / 32u, (u32(mip0.y) + 31u) / 32u);
                    for (u32 level = mip_count - 1u; level >= 1u; --level) {
                        const int2 dst = RetroPass::bloomMipExtent(mip0, level - 1u);
                        params.m_upsample_level = level;
                        PPR_TEST_ASSERT(not make_error_code(up_constants.setData(&params, sizeof(params))));
                        PPR_TEST_ASSERT(not make_error_code(
                            up_cursor["g_up_src"].setBinding(rhi::Binding(m_bloom_uav_views[level]))));
                        PPR_TEST_ASSERT(not make_error_code(
                            up_cursor["g_up_dst"].setBinding(rhi::Binding(m_bloom_uav_views[level - 1u]))));
                        encoder->globalBarrier();
                        dispatch(2u, (u32(dst.x) + 7u) / 8u, (u32(dst.y) + 7u) / 8u);
                    }

                    encoder->setTextureState(m_bloom, chain, rhi::ResourceState::ShaderResource);
                    submit(*encoder);

                    shader::ComPtr<ISlangBlob> blob;
                    rhi::SubresourceLayout layout{};
                    PPR_TEST_ASSERT(not make_error_code(device.readTexture(m_bloom, 0u, 0u, blob.writeRef(), &layout)));
                    const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer());

                    std::vector<std::array<float, 3>> result(std::size_t(mip0.x) * std::size_t(mip0.y));
                    for (u32 y = 0u; y < u32(mip0.y); ++y) {
                        for (u32 x = 0u; x < u32(mip0.x); ++x) {
                            const std::byte *const pixel = bytes + y * layout.rowPitch + x * 8u;
                            std::array<u16, 4> halves{};
                            std::memcpy(halves.data(), pixel, sizeof(halves));
                            result[std::size_t(y) * std::size_t(mip0.x) + x] = {
                                WorldLightFieldGpu::halfToFloat(halves[0]), WorldLightFieldGpu::halfToFloat(halves[1]),
                                WorldLightFieldGpu::halfToFloat(halves[2])};
                        }
                    }

                    scene_view.setNull();
                    scene_tex.setNull();
                    encoder.setNull();
                    return result;
                }

                void shutdown() {
                    if (m_queue and make_error_code(m_queue->waitOnHost()))
                        hal::fatalExit("spd-bloom-test-quiescence-unknown");

                    m_roots = {};
                    m_pipelines = {};
                    m_programs = {};
                    m_bloom_uav_views = {};
                    m_bloom_view.setNull();
                    m_bloom.setNull();
                    PPR_TEST_ASSERT(not m_fence.shutdown());
                    m_queue.setNull();
                    PPR_TEST_ASSERT(not m_rhi->shutdown());
                    PPR_TEST_ASSERT(not m_shaders->shutdown());
                }
            };

            [[nodiscard]] float luminanceOf(const std::array<float, 3> &pixel) noexcept {
                return 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
            }

            PPR_UNIT_TEST (gpu_bloom_smoke) {
                constexpr u32 edge = 128u;

                Fixture fixture;
                fixture.initialize(edge);
                PPR_DEFER {
                    fixture.shutdown();
                };

                const int2 mip0 = RetroPass::bloomMip0Extent(int2{int(edge), int(edge)});
                const auto at = [&](const std::vector<std::array<float, 3>> &result, const u32 x, const u32 y) {
                    return luminanceOf(result[std::size_t(y) * u32(mip0.x) + x]);
                };

                const auto bright = fixture.run(edge, 0.05f, 20.0f);
                PPR_TEST_ASSERT(bright.size() == std::size_t(mip0.x) * std::size_t(mip0.y));

                // Finite everywhere, and the bright block leaves a non-zero trail.
                bool all_finite = true;
                for (const auto &pixel: bright) {
                    all_finite = all_finite and
                                 std::isfinite(pixel[0]) and
                                 std::isfinite(pixel[1]) and
                                 std::isfinite(pixel[2]);
                }
                PPR_TEST_ASSERT(all_finite);

                // The block is 16 scene texels centred at edge/2, so in mip 0 it
                // spans (edge/4-2 .. edge/4+1); its centre is clearly lit, the
                // texel just outside is lit ("around the block"), and a texel
                // seven away is still lit — far beyond the two-texel reach of a
                // single 5x5 binomial blur, which a wider multi-mip chain reaches.
                const u32 block_centre = edge / 4u;
                PPR_TEST_ASSERT(at(bright, block_centre, block_centre) > 0.01f);
                PPR_TEST_ASSERT(at(bright, block_centre + 2u, block_centre) > 1e-3f);
                PPR_TEST_ASSERT(at(bright, block_centre + 8u, block_centre) > 1e-4f);
                PPR_TEST_ASSERT(at(bright, block_centre, block_centre) > at(bright, block_centre + 8u, block_centre));

                // A black picture stays black: the soft-knee bright pass emits zero.
                const auto black = fixture.run(edge, 0.0f, 0.0f);
                for (const auto &pixel: black) {
                    PPR_TEST_ASSERT(std::abs(pixel[0]) < 1e-6f);
                    PPR_TEST_ASSERT(std::abs(pixel[1]) < 1e-6f);
                    PPR_TEST_ASSERT(std::abs(pixel[2]) < 1e-6f);
                }
            };
        }

        const UnitTest spd_bloom = UnitTest::Named("spd_bloom") / [](UnitTest::IRun &run) -> void {
            run.recurse({cpu_bloom_chain_layout, Gpu::gpu_bloom_smoke});
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
            detail::WorldLightFieldTests::producer_stages_only_dirty_chunks_and_advances_revision,
            detail::WorldLightGpu::gpu_source_matter_overlay,
            detail::WorldLightGpu::gpu_source_over_capacity_update_preserves_state,
            detail::WorldLightGpu::gpu_source_second_update_replaces_pending_particles,
            detail::WorldLightGpu::gpu_source_multiword_occupancy,
            detail::WorldLightFieldGpu::field_single_emitter_converges,
            detail::WorldLightFieldGpu::field_accumulation_caps_count_and_retention_holds_history,
            detail::WorldLightFieldGpu::field_dirty_update_reprocesses_in_same_frame,
            detail::WorldLightFieldGpu::field_reset_bounds_rect_tail,
            detail::WorldLightFieldGpu::field_update_respects_budget_boundary,
            detail::WorldLightFieldGpu::field_dense_dda_hits_diagonal_emitter,
            detail::WorldLightFieldGpu::field_is_camera_independent,
            detail::WorldLightFieldGpu::field_routing_applies_dirty_chunk_and_binds_view,
            detail::WorldLightFieldGpu::composite_matches_cpu_reference,
            detail::WorldLightFieldGpu::diffuse_plus_z_isotropic_and_perturbed_finite,
            detail::WorldLightFieldGpu::specular_vertical_reflection_and_roughness_finite,
            detail::WorldLightFieldGpu::grid_shading_uses_field,
            detail::WorldLightFieldGpu::translucent_clipped_by_terrain,
            detail::WorldLightFieldGpu::translucent_premultiplied_absorption,
            detail::HistogramAdaptation::histogram_adaptation,
            detail::SpdBloom::spd_bloom,
        });
    };

    const UnitTest &world_light_fieldTests() noexcept {
        return world_light_field_contracts;
    }
}
