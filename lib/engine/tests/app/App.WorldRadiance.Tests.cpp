module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <slang.h>
#include <cstdio>

module engine.tests.app;

import engine.core;
import engine.app;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.math;
import std;

// Existing fixed mask/source builder, not a copied or adjusted scene. The new
// transport reference projects these SAME labels onto native half-open cells.
namespace pP::tests::detail::RadianceGpu {
    std::shared_ptr<const lighting::TerrainReceiverSet> backgroundReceivers_(bool constant, bool open);
}

namespace pP::tests::detail::WorldRadianceGpu {
    // ------------------------------------------------------------------
    // full-world temporal profile admission without GPU allocation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (temporal_world_profile_admission) {
        const lighting::WorldRadianceTemporalSettings settings{};
        const auto profile = lighting::planWorldRadianceTemporalProfile(4096u, 4096u, settings);
        PPR_TEST_ASSERT(profile.has_value());
        PPR_TEST_ASSERT(settings.m_refined_spacing == 4u and settings.m_cascade_count == 6u);
        PPR_TEST_ASSERT(profile->m_refined_counts == (std::array<u32, 2>{1026u, 1026u}));
        PPR_TEST_ASSERT(profile->m_refined_bytes == 117899712u);
        PPR_TEST_ASSERT(profile->m_probe_state_bytes == 1026ull * 1026u * 24u);
        PPR_TEST_ASSERT(profile->m_field_pool_bytes == 3u * (profile->m_refined_bytes + profile->m_probe_state_bytes));
        PPR_TEST_ASSERT(profile->m_cascade_bytes + profile->m_field_pool_bytes < 4ull * 1024u * 1024u * 1024u);
        PPR_TEST_ASSERT(profile->m_levels[5].m_far_cells >= 2.0f * (4096.0f + 4096.0f));

        for (u32 level = 1u; level < settings.m_cascade_count; ++level) {
            const auto &row = profile->m_levels[level];
            PPR_TEST_ASSERT(row.m_records == u64{row.m_counts[0]} * row.m_counts[1] * row.m_directions);
            PPR_TEST_ASSERT(row.m_minimum[0] < 0 and row.m_minimum[1] < 0);
            PPR_TEST_ASSERT(row.m_first_record + row.m_records <= profile->m_cascade_records);
            PPR_TEST_ASSERT(row.m_near_cells < row.m_far_cells);
        }

        std::println(
            "WORLD_TEMPORAL_CAPACITY spacing={} levels={} cascade_bytes={} field_pool_bytes={} refined_bytes={}",
            settings.m_refined_spacing, settings.m_cascade_count, profile->m_cascade_bytes, profile->m_field_pool_bytes,
            profile->m_refined_bytes);

        auto invalid = settings;
        invalid.m_refined_spacing = 3u;
        PPR_TEST_ASSERT(not lighting::planWorldRadianceTemporalProfile(4096u, 4096u, invalid));
        invalid = settings;
        invalid.m_interval_scale = 0.0f;
        PPR_TEST_ASSERT(not lighting::planWorldRadianceTemporalProfile(4096u, 4096u, invalid));
        PPR_TEST_ASSERT(not lighting::planWorldRadianceTemporalProfile(0u, 4096u, settings));
        PPR_TEST_ASSERT(not lighting::planWorldRadianceTemporalProfile(4097u, 4096u, settings));

        for (const u32 edge: {1024u, 2048u}) {
            const auto smaller = lighting::planWorldRadianceTemporalProfile(edge, edge, settings);
            PPR_TEST_ASSERT(smaller and smaller->m_cascade_bytes < profile->m_cascade_bytes);
        }
    };

    // ------------------------------------------------------------------
    // one live candidate producer with the existing owner queue and fence
    // ------------------------------------------------------------------

    struct Query {
        float x{}, y{}, dx{}, dy{}, near_cells{}, far_cells{};
        u32 terminal{}, reserved{};
    };
    struct Interval {
        float source[3]{};
        u32 status{};
        float transmission[3]{};
        u32 segments{};
    };
    struct Material {
        float source[3]{};
        u32 kind{3u};
        float sigma[3]{}, emission[3]{};
        float m_albedo[3]{};
    };
    struct Parameters {
        u32 width{}, height{}, palette{}, hierarchy_levels{};
        float sky[3]{};
        u32 max_segments{};
        i32 refined_min_x{}, refined_min_y{};
        u32 refined_count_x{}, refined_count_y{};
        float max_source[3]{};
        float maximum_response{(std::numeric_limits<float>::max)()};
        float cell_metres{1.0f};
        float far_bound[3]{};
        u32 overlay_count{}, overlay_capacity{}, overlay_ancestor_words{}, sky_hour_plus_one{};
    };
    struct Dispatch {
        u32 level{}, parent_level{}, count{}, reserved{};
        i32 minimum_x{}, minimum_y{};
        u32 count_x{}, count_y{};
    };
    struct BounceDispatch {
        u32 faces{}, capacity{}, first{}, reserved{};
    };
    struct Hierarchy {
        u32 offset{}, width{}, height{}, block_size{};
    };
    struct NormalQuery {
        float x{}, y{}, z{};
        u32 coefficient{};
    };
    struct NormalResult {
        float value[3]{};
        u32 status{};
        float display[3]{};
        u32 reserved{};
    };

    static_assert(sizeof(Query) == 32u and sizeof(Interval) == 32u and sizeof(Material) == 52u);
    static_assert(PPR_OFFSETOF(Material, m_albedo) == 40u);
    static_assert(sizeof(Parameters) == 96u and sizeof(Dispatch) == 32u and sizeof(Hierarchy) == 16u);
    static_assert(sizeof(BounceDispatch) == 16u);
    static_assert(PPR_OFFSETOF(Parameters, maximum_response) == 60u);
    static_assert(PPR_OFFSETOF(Parameters, overlay_ancestor_words) == 88u);
    static_assert(sizeof(NormalQuery) == 16u and sizeof(NormalResult) == 32u);
    static_assert(PPR_OFFSETOF(Interval, transmission) == 16u and PPR_OFFSETOF(NormalResult, status) == 12u);
    static_assert(PPR_OFFSETOF(NormalResult, display) == 16u);

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue;
        FrameFence m_fence;
        lighting::WorldRadiancePass m_pass;
        lighting::WorldRadiancePass m_matter_pass;
        GridPass m_grid;
        shader::SharedModule m_module, m_grid_module;
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 8> m_programs;
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 8> m_pipelines;
        rhi::ComPtr<rhi::IBuffer> m_unused_face_keys, m_unused_face_lookup, m_unused_face_moments;
        rhi::ComPtr<rhi::IBuffer> m_unused_overlay, m_unused_overlay_lookup;
        bool m_shaders_attempted{}, m_rhi_attempted{};

        void initialize() {
            m_shaders_attempted = true;
            PPR_TEST_ASSERT(not m_shaders->initialize());
            const auto setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
            const bool validation = setting and *setting == "1";
            // Ordinary runs retain Core checks; the flag selects GPUAV only.
            m_rhi->setCoreValidation(true);
            m_rhi->setGpuAssistedValidation(validation);
#ifdef _WIN32
            constexpr auto backend = rhi::DeviceType::D3D12;
#else
            constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
            m_rhi_attempted = true;
            PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
            auto &device = m_rhi->getDevice();
            PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));
            PPR_TEST_ASSERT(not m_fence.initialize(device));

            // One actual raster-module initialization shares this RHI fixture.
            // Compute-only stride checks cannot catch nested draw ABI rejection.
            PPR_TEST_ASSERT(not m_grid.initialize(*m_rhi, *m_shaders, std::filesystem::current_path()));

            PPR_TEST_ASSERT(not m_pass.initialize(device, *m_shaders, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not m_matter_pass.initialize(device, *m_shaders, std::filesystem::current_path()));

            PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "world_radiance.slang", "world_radiance",
                m_module.writeRef()));
            for (const auto *const name:
                {"refineWorldRadiance", "refineWorldRadianceParallel", "refineWorldBounce",
                    "bakeWorldFaceMomentsParallel"}) {
                shader::ComPtr<slang::IEntryPoint> entry;
                PPR_TEST_ASSERT(not make_error_code(m_module->findEntryPointByName(name, entry.writeRef())));
                auto *const entry_layout = entry->getLayout();
                PPR_TEST_ASSERT(entry_layout != nullptr and entry_layout->getEntryPointCount() == 1u);

                std::array<SlangUInt, 3u> group{};
                entry_layout->getEntryPointByIndex(0u)->getComputeThreadGroupSize(group.size(), group.data());
                PPR_TEST_ASSERT(group == (std::array<SlangUInt, 3u>{64u, 1u, 1u}));
            }

            auto *const layout = m_module->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);
            for (const auto &[name, stride]: std::array<std::pair<const char *, u32>, 6u>{
                     std::pair{"g_overlay", 56u}, {"g_queries", 32u}, {"g_current", 32u},
                     {"g_normal_coefficients", 112u}, {"g_normal_queries", 16u}, {"g_normal_results", 32u}}) {
                slang::TypeLayoutReflection *element{};
                for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                    auto *const parameter = layout->getParameterByIndex(index);
                    if (std::string_view{parameter->getName()} == name) {
                        element = parameter->getTypeLayout()->getElementTypeLayout();
                        break;
                    }
                }

                PPR_TEST_ASSERT(element != nullptr and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == stride);
            }

            PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "grid_tiles.slang", "grid_tiles",
                m_grid_module.writeRef()));
            auto *const grid_layout = m_grid_module->getLayout();
            PPR_TEST_ASSERT(grid_layout != nullptr);
            for (const auto &[name, stride]: std::array<std::pair<const char *, u32>, 2u>{
                     std::pair{"g_grid_world_radiance", 48u}, {"g_grid_world_coefficients", 112u}}) {
                slang::TypeLayoutReflection *element{};
                for (unsigned index = 0u; index < grid_layout->getParameterCount(); ++index) {
                    auto *const parameter = grid_layout->getParameterByIndex(index);
                    if (std::string_view{parameter->getName()} == name) {
                        element = parameter->getTypeLayout()->getElementTypeLayout();
                        break;
                    }
                }

                PPR_TEST_ASSERT(element != nullptr and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == stride);
            }

            const u32 zero{};
            const std::array<float, 3u> zero_moment{};
            const lighting::WorldRadianceOverlayCell empty_overlay{};
            m_unused_overlay = buffer(sizeof(empty_overlay), sizeof(empty_overlay), &empty_overlay, false, true);
            m_unused_overlay_lookup = buffer(sizeof(zero), sizeof(u32), &zero, false, true);
            m_unused_face_keys = buffer(sizeof(zero), sizeof(u32), &zero);
            m_unused_face_lookup = buffer(sizeof(zero), sizeof(u32), &zero);
            m_unused_face_moments = buffer(sizeof(zero_moment), sizeof(zero_moment), zero_moment.data());

            const std::array<const char *, 8> entries{
                "traceWorldQueries", "qualifyWorldDiffuse", "qualifyGridWorldRadiance", "buildWorldFaceLookup",
                "bakeWorldFaceMoments", "bakeWorldFaceMomentsParallel", "buildWorldSkipTable", "traceWorldSkipQueries"};
            for (u32 index = 0u; index < entries.size(); ++index) {
                shader::ComPtr<slang::IEntryPoint> entry;
                auto *const module = index == 2u ? m_grid_module.get() : m_module.get();
                PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entries[index], entry.writeRef())));

                slang::IComponentType *points[]{entry.get()};
                rhi::ShaderProgramDesc description{};
                description.linkingStyle = rhi::LinkingStyle::SingleProgram;
                description.slangGlobalScope = module;
                description.slangEntryPoints = points;
                description.slangEntryPointCount = 1u;
                shader::Diagnose diagnostics;
                PPR_TEST_ASSERT(not make_error_code(
                    device.createShaderProgram(description, m_programs[index].writeRef(), diagnostics.writeRef())));

                rhi::ComputePipelineDesc pipeline{};
                pipeline.program = m_programs[index];
                PPR_TEST_ASSERT(
                    not make_error_code(device.createComputePipeline(pipeline, m_pipelines[index].writeRef())));
            }
        }

        rhi::ComPtr<rhi::IBuffer> buffer(
            const u64 bytes, const u32 stride, const void *const initial = nullptr, const bool readback = false,
            const bool immutable = false) {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = readback ? 0u : stride;
            description.memoryType = readback ? rhi::MemoryType::ReadBack : rhi::MemoryType::DeviceLocal;
            description.usage = readback ? rhi::BufferUsage::CopyDestination
                                         : static_cast<rhi::BufferUsage>(
                                               static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                                               static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                                               static_cast<u32>(rhi::BufferUsage::CopySource) |
                                               static_cast<u32>(rhi::BufferUsage::CopyDestination));
            description.defaultState = readback    ? rhi::ResourceState::CopyDestination
                                       : immutable ? rhi::ResourceState::ShaderResource
                                                   : rhi::ResourceState::UnorderedAccess;
            description.label = "World RC fixed qualification input/output";
            rhi::ComPtr<rhi::IBuffer> result;
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().createBuffer(description, initial, result.writeRef())));
            return result;
        }

        void bind(rhi::ShaderCursor &cursor, const char *const name, rhi::IBuffer *const value) {
            PPR_TEST_ASSERT(not make_error_code(
                cursor[name].setBinding(
                    rhi::Binding(value, rhi::BufferRange{.offset = 0u, .size = value->getDesc().size}))));
        }

        void constant(rhi::ShaderCursor &cursor, const char *const name, const void *const value, const u64 bytes) {
            if (std::string_view{name} == "g_world") {
                bind(cursor, "g_overlay", m_unused_overlay);
                bind(cursor, "g_overlay_lookup", m_unused_overlay_lookup);
                bind(cursor, "g_overlay_ancestors", m_unused_overlay_lookup);
                const std::array<u32, 8u> unused_discovery{};
                constant(cursor, "g_discovery", unused_discovery.data(), sizeof(unused_discovery));
                bind(cursor, "g_discovery_bits", m_unused_face_lookup);
                bind(cursor, "g_discovery_keys", m_unused_face_lookup);
                bind(cursor, "g_discovery_state", m_unused_face_lookup);
            }

            rhi::ShaderCursor contents{};
            PPR_TEST_ASSERT(not make_error_code(cursor[name].getDereferenced(contents)));
            PPR_TEST_ASSERT(not make_error_code(contents.setData(value, bytes)));
        }

        // Primary qualification roots still own valid bindings for the inactive
        // secondary branch. No moment/table getter or producer mutation is used.
        void bindUnusedBounce(rhi::ShaderCursor &cursor) {
            const BounceDispatch unused{};
            constant(cursor, "g_bounce", &unused, sizeof(unused));
            bind(cursor, "g_face_keys", m_unused_face_keys);
            bind(cursor, "g_face_lookup", m_unused_face_lookup);
            bind(cursor, "g_face_moments", m_unused_face_moments);
        }

        void submit(
            rhi::ICommandEncoder &encoder, const bool producer, const bool wait = true,
            lighting::WorldRadiancePass *const producer_owner = nullptr) {
            auto &owner = producer_owner != nullptr ? *producer_owner : m_pass;

            rhi::ComPtr<rhi::ICommandBuffer> commands;
            const auto finished = make_error_code(encoder.finish(commands.writeRef()));
            if (finished and producer)
                PPR_TEST_ASSERT(not owner.discardUnsubmitted());
            PPR_TEST_ASSERT(not finished);

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

            const auto submitted = make_error_code(m_queue->submit(description));
            if (submitted and producer)
                PPR_TEST_ASSERT(not owner.notifySubmitFailed());
            PPR_TEST_ASSERT(not submitted);
            m_fence.commitSubmitted(value);
            if (producer)
                PPR_TEST_ASSERT(not owner.notifySubmitted(value));

            if (wait) {
                PPR_TEST_ASSERT(not m_fence.waitFor(value));
            }
        }

        template<typename T>
        std::vector<T> read(rhi::IBuffer *const source) {
            auto copy = buffer(source->getDesc().size, 0u, nullptr, true);
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
            encoder->setBufferState(source, rhi::ResourceState::CopySource);
            encoder->copyBuffer(copy, 0u, source, 0u, source->getDesc().size);
            encoder->setBufferState(source, rhi::ResourceState::ShaderResource);
            submit(*encoder, false);

            std::vector<T> result(source->getDesc().size / sizeof(T));
            void *mapped{};
            PPR_TEST_ASSERT(not make_error_code(m_rhi->getDevice().mapBuffer(copy, rhi::CpuAccessMode::Read, &mapped)));
            std::memcpy(result.data(), mapped, result.size() * sizeof(T));
            PPR_TEST_ASSERT(not make_error_code(m_rhi->getDevice().unmapBuffer(copy)));
            return result;
        }

        std::shared_ptr<const lighting::WorldRadianceGeneration> generate(
            const std::shared_ptr<const lighting::WorldRadianceSource> &source, const u64 revision,
            const bool reject_replacement = false,
            const lighting::WorldRadianceRefinedSpacing spacing =
                lighting::WorldRadianceRefinedSpacing::two_cell_centres,
            const u32 cascade_count = 4u,
            const lighting::WorldRadianceTransport transport = lighting::WorldRadianceTransport::cascade_diagnostic,
            const float maximum_response = (std::numeric_limits<float>::max)(), const bool measure_gpu = false,
            lighting::WorldRadianceReceiverWindow window = {}, const std::optional<u32> expected_batch_count = {},
            const u32 receiver_rows_per_quantum = 256u) {
            if (transport == lighting::WorldRadianceTransport::direct_window and window.m_counts[0] == 0u) {
                window.m_counts = {source->width(), source->height()};
            }

            const lighting::WorldRadianceSettings settings{
                64u * 1024u * 1024u, 1024u * 1024u, 32u * 1024u * 1024u, cascade_count, 0.5f, true, measure_gpu,
                spacing, transport, maximum_response, window, receiver_rows_per_quantum};
            const lighting::TerrainReceiverCohort cohort{7u, 11u, 13u, revision};

            const auto admitted = m_pass.prepare(source, cohort, settings, m_fence.completed());
            PPR_TEST_ASSERT(admitted.has_value() and not admitted->m_admission_error);
            std::println(
                "WORLD_RC_ADMISSION revision={} refined_spacing={} configured_cascades={} transport={} coarse_dispatches={} parity_even={} parity_odd={} refined={} source={} readback={} peak={} queries_bound={} expected_rows={}",
                revision, static_cast<u32>(spacing), cascade_count, static_cast<u32>(transport),
                transport == lighting::WorldRadianceTransport::cascade_diagnostic ? cascade_count - 1u : 0u,
                admitted->m_parity_bytes[0], admitted->m_parity_bytes[1], admitted->m_refined_bytes,
                admitted->m_source_gpu_bytes, admitted->m_readback_bytes, admitted->m_peak_bytes,
                admitted->m_query_upper_bound, admitted->m_expected_rows);

            if (transport == lighting::WorldRadianceTransport::straight256_reference) {
                const u64 cells = u64{source->width()} * source->height();
                PPR_TEST_ASSERT(admitted->m_expected_rows == cells);
                PPR_TEST_ASSERT(admitted->m_query_upper_bound == cells * 256u);
                PPR_TEST_ASSERT(admitted->m_parity_bytes[0] == 0u and admitted->m_parity_bytes[1] == sizeof(Interval));
            }

            if (reject_replacement) {
                auto replacement_settings = settings;
                replacement_settings.m_budget_bytes = 1u;
                const auto replacement = m_pass.prepare(
                    source, {7u, 11u, 13u, revision + 1u}, replacement_settings, m_fence.completed());
                PPR_TEST_ASSERT(
                    replacement.has_value() and replacement->m_admission_error == std::errc::no_buffer_space);
                // The ORIGINAL never-submitted batch remains encodable. Its
                // accepted submission below must require fresh completion proof.
            }

            if (transport == lighting::WorldRadianceTransport::direct_window) {
                const u32 expected_batches = m_pass.pendingBatches();
                std::println(
                    "WORLD_WINDOW_QUOTA revision={} receiver_rows_per_quantum={} pending_batches={} expected_batches={}",
                    revision, receiver_rows_per_quantum, expected_batches,
                    expected_batch_count.value_or(expected_batches));
                PPR_TEST_ASSERT(expected_batches > 0u);
                if (expected_batch_count)
                    PPR_TEST_ASSERT(expected_batches == *expected_batch_count);

                for (u32 page = 0u; page < expected_batches; ++page) {
                    PPR_TEST_ASSERT(m_pass.canEncodeNextBatch());
                    rhi::ComPtr<rhi::ICommandEncoder> page_encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(page_encoder.writeRef())));
                    PPR_TEST_ASSERT(not m_pass.encode({m_rhi->getDevice(), *page_encoder, m_fence.completed()}));

                    submit(*page_encoder, true);
                    PPR_TEST_ASSERT(not m_pass.canEncodeNextBatch());
                    const auto premature = m_pass.acquire(kFenceValueNone);
                    PPR_TEST_ASSERT(not premature and premature.error() == std::errc::device_or_resource_busy);
                    if (measure_gpu) {
                        // Only this fixture waits for backend query-host readiness.
                        PPR_TEST_ASSERT(not make_error_code(m_queue->waitOnHost()));
                    }

                    const auto ready = m_pass.advance(m_fence.completed());
                    PPR_TEST_ASSERT(ready.has_value() and *ready == (page + 1u == expected_batches));
                    PPR_TEST_ASSERT(m_pass.pendingBatches() == expected_batches - page - 1u);
                }

                const auto generation = m_pass.acquire(m_fence.completed());
                PPR_TEST_ASSERT(generation.has_value());
                PPR_TEST_ASSERT((*generation)->probeMinimum() == window.m_minimum);
                PPR_TEST_ASSERT((*generation)->probeCounts() == window.m_counts);
                PPR_TEST_ASSERT(
                    (*generation)->statistics().m_rows_written == u64{window.m_counts[0]} * window.m_counts[1]);
                const auto repeated = m_pass.acquire(m_fence.completed());
                PPR_TEST_ASSERT(repeated.has_value() and *repeated == *generation);
                return *generation;
            }

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
            PPR_TEST_ASSERT(not m_pass.encode({m_rhi->getDevice(), *encoder, m_fence.completed()}));

            if (measure_gpu) {
                // A definitely-unsubmitted encoder may be discarded and retried
                // without publishing timing or stealing a GPU fence identity.
                PPR_TEST_ASSERT(not m_pass.discardUnsubmitted());
                encoder.setNull();
                const auto before_submit = m_pass.pollTiming(cohort, m_fence.completed());
                PPR_TEST_ASSERT(before_submit.m_submission == kFenceValueNone);
                PPR_TEST_ASSERT(not before_submit.m_milliseconds);

                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not m_pass.encode({m_rhi->getDevice(), *encoder, m_fence.completed()}));
            }

            submit(*encoder, true);
            const auto premature = m_pass.acquire(kFenceValueNone);
            PPR_TEST_ASSERT(not premature and premature.error() == std::errc::device_or_resource_busy);
            const auto premature_timing = m_pass.pollTiming(cohort, kFenceValueNone);
            PPR_TEST_ASSERT(not premature_timing.m_milliseconds);
            PPR_TEST_ASSERT(premature_timing.m_cohort == cohort);
            PPR_TEST_ASSERT(premature_timing.m_submission == m_fence.lastSubmitted());
            if (measure_gpu and m_rhi->getDevice().getInfo().timestampFrequency != 0u) {
                PPR_TEST_ASSERT(premature_timing.m_status == lighting::WorldRadianceTimingStatus::pending);
            } else if (not measure_gpu) {
                PPR_TEST_ASSERT(premature_timing.m_status == lighting::WorldRadianceTimingStatus::disabled);
            }

            const auto generation = m_pass.acquire(m_fence.completed());
            PPR_TEST_ASSERT(generation.has_value());

            if (measure_gpu) {
                // Test-only host proof: D3D12 signals the app fence before its
                // internal query-tracking fence. Production polling must remain
                // nonblocking, so only this numeric assertion waits for queue idle.
                PPR_TEST_ASSERT(not make_error_code(m_queue->waitOnHost()));
            }

            const auto timing = m_pass.pollTiming(cohort, m_fence.completed());
            PPR_TEST_ASSERT(timing.m_submission == m_fence.lastSubmitted());
            if (measure_gpu and m_rhi->getDevice().getInfo().timestampFrequency != 0u) {
                PPR_TEST_ASSERT(timing.m_status == lighting::WorldRadianceTimingStatus::ready);
                PPR_TEST_ASSERT(not timing.m_error and timing.m_milliseconds);
                PPR_TEST_ASSERT(std::isfinite(*timing.m_milliseconds) and *timing.m_milliseconds >= 0.0);
            } else {
                PPR_TEST_ASSERT(not timing.m_milliseconds);
                PPR_TEST_ASSERT(
                    timing.m_status == (measure_gpu ? lighting::WorldRadianceTimingStatus::unsupported
                                                    : lighting::WorldRadianceTimingStatus::disabled));
            }

            PPR_TEST_ASSERT((*generation)->refinedSpacing() == spacing);
            PPR_TEST_ASSERT((*generation)->transport() == transport);
            if (transport == lighting::WorldRadianceTransport::straight256_reference) {
                const auto &statistics = (*generation)->statistics();
                PPR_TEST_ASSERT((*generation)->probeMinimum() == (std::array<i32, 2>{0, 0}));
                PPR_TEST_ASSERT(
                    (*generation)->probeCounts() == (std::array<u32, 2>{source->width(), source->height()}));
                PPR_TEST_ASSERT(statistics.m_queries_attempted == statistics.m_query_upper_bound);
            }

            const auto repeated = m_pass.acquire(m_fence.completed());
            PPR_TEST_ASSERT(repeated.has_value() and *repeated == *generation);
            return *generation;
        }

        std::tuple<
            std::shared_ptr<const lighting::WorldRadianceGeneration>,
            std::shared_ptr<const lighting::WorldRadianceGeneration>,
            std::shared_ptr<const lighting::WorldRadianceFaceDiscovery>>
        generateBounce(
            const std::shared_ptr<const lighting::WorldRadianceSource> &source, const u64 revision,
            const lighting::WorldRadianceReceiverWindow window, const u64 epoch, const bool corrupt_keys = false,
            lighting::WorldRadianceSupportCounts *const support_counts = nullptr) {
            const auto canceled_bounce = m_pass.cancelBounce(m_fence.completed());
            PPR_TEST_ASSERT(canceled_bounce.has_value() and *canceled_bounce);
            const auto canceled_discovery = m_pass.cancelDiscovery(m_fence.completed());
            PPR_TEST_ASSERT(canceled_discovery.has_value() and *canceled_discovery);

            const auto direct = generate(
                source, revision, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
                lighting::WorldRadianceTransport::direct_window, 65504.0f, false, window);
            const lighting::WorldRadianceDiscoverySettings discovery_settings{
                64ull * 1024u * 1024u, 1024ull * 1024u, 32ull * 1024u * 1024u, epoch, window, 0u, false,
                support_counts != nullptr};
            const auto discovery_demand = m_pass.prepareDiscovery(
                source, direct->cohort(), discovery_settings, m_fence.completed());
            PPR_TEST_ASSERT(discovery_demand.has_value() and not discovery_demand->m_admission_error);

            u64 frame = epoch * 100000u;
            for (u32 index = 0u; index < discovery_demand->m_batches; ++index) {
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(
                    not m_pass.encodeDiscovery({m_rhi->getDevice(), *encoder, m_fence.completed()}, frame++));
                submit(*encoder, true);
                const auto ready = m_pass.advanceDiscovery(m_fence.completed());
                PPR_TEST_ASSERT(ready.has_value() and *ready == (index + 1u == discovery_demand->m_batches));
            }

            const auto discovered = m_pass.acquireDiscovery(m_fence.completed());
            PPR_TEST_ASSERT(discovered.has_value());
            const auto retired = m_pass.cancelDiscovery(m_fence.completed());
            PPR_TEST_ASSERT(retired.has_value() and *retired);

            if (corrupt_keys) {
                PPR_TEST_ASSERT((*discovered)->faceCount() > 0u);
                // Intentional isolated corruption AFTER the completed discovery
                // fence. No other case uses this immutable result afterwards.
                const u32 invalid = 0xffffffffu;
                const auto invalid_buffer = buffer(sizeof(invalid), sizeof(u32), &invalid);
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                encoder->setBufferState(invalid_buffer, rhi::ResourceState::CopySource);
                encoder->setBufferState((*discovered)->keysBuffer(), rhi::ResourceState::CopyDestination);
                encoder->copyBuffer((*discovered)->keysBuffer(), 0u, invalid_buffer, 0u, sizeof(invalid));
                submit(*encoder, false); // Wait preserves the upload source and exact write fence.
            }

            const lighting::WorldRadianceBounceSettings settings{
                64ull * 1024u * 1024u, 1024ull * 1024u, 32ull * 1024u * 1024u, epoch, false};
            const auto demand = m_pass.prepareBounce(*discovered, settings, m_fence.completed());
            PPR_TEST_ASSERT(demand.has_value() and not demand->m_admission_error);
            if (support_counts) {
                PPR_TEST_ASSERT(demand->m_support_counts.has_value());

                *support_counts = *demand->m_support_counts;
            }

            PPR_TEST_ASSERT(
                demand->m_trace_upper_bound ==
                (*discovered)->statistics().m_query_upper_bound + demand->m_faces * 128u);
            PPR_TEST_ASSERT(
                demand->m_record_count == (*discovered)->statistics().m_expected_rows +
                                              2u * demand->m_faces +
                                              u64{window.m_counts[0]} * window.m_counts[1]);
            PPR_TEST_ASSERT(demand->m_lookup_capacity >= 2u * demand->m_faces);
            PPR_TEST_ASSERT(demand->m_moment_bytes == std::max<u64>(1u, demand->m_faces) * 12u);

            for (u32 index = 0u; index < demand->m_batches; ++index) {
                PPR_TEST_ASSERT(m_pass.canEncodeBounce());
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not m_pass.encodeBounce({m_rhi->getDevice(), *encoder, m_fence.completed()}, frame++));
                submit(*encoder, true, false);
                const auto premature = m_pass.acquireBounce(kFenceValueNone);
                PPR_TEST_ASSERT(not premature and premature.error() == std::errc::device_or_resource_busy);
                const auto busy = m_pass.cancelBounce(kFenceValueNone);
                PPR_TEST_ASSERT(busy.has_value() and not *busy);

                PPR_TEST_ASSERT(not m_fence.waitFor(m_fence.lastSubmitted()));
                const auto ready = m_pass.advanceBounce(m_fence.completed());
                if (corrupt_keys) {
                    PPR_TEST_ASSERT(not ready and ready.error() == std::errc::result_out_of_range);
                    const auto unavailable = m_pass.acquireBounce(m_fence.completed());
                    PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::device_or_resource_busy);
                    const auto canceled = m_pass.cancelBounce(m_fence.completed());
                    PPR_TEST_ASSERT(canceled.has_value() and *canceled);
                    return {direct, {}, *discovered};
                }

                PPR_TEST_ASSERT(ready.has_value() and *ready == (index + 1u == demand->m_batches));
            }

            const auto combined = m_pass.acquireBounce(m_fence.completed());
            PPR_TEST_ASSERT(combined.has_value());
            PPR_TEST_ASSERT((*combined)->source() == direct->source() and (*combined)->cohort() == direct->cohort());
            PPR_TEST_ASSERT(
                (*combined)->probeMinimum() == window.m_minimum and (*combined)->probeCounts() == window.m_counts);
            PPR_TEST_ASSERT(
                (*combined)->bounceEpoch() == epoch and (*combined)->completionFence() == m_fence.lastSubmitted());
            PPR_TEST_ASSERT((*combined)->statistics().m_queries_completed == demand->m_trace_upper_bound);
            const auto canceled = m_pass.cancelBounce(m_fence.completed());
            PPR_TEST_ASSERT(canceled.has_value() and *canceled);
            return {direct, *combined, *discovered};
        }

        void shutdown() {
            if (m_queue and make_error_code(m_queue->waitOnHost()))
                hal::fatalExit("world-radiance-test-quiescence-unknown");

            const auto matter_error = m_matter_pass.shutdown({GpuQuiescence::kIdleProven, {}});
            const auto pass_error = m_pass.shutdown({GpuQuiescence::kIdleProven, {}});
            const auto grid_error = m_grid.shutdown();
            for (auto &pipeline: m_pipelines)
                pipeline.setNull();
            for (auto &program: m_programs)
                program.setNull();

            m_unused_overlay_lookup.setNull();
            m_unused_overlay.setNull();
            m_unused_face_moments.setNull();
            m_unused_face_lookup.setNull();
            m_unused_face_keys.setNull();
            m_module = {};
            m_grid_module = {};
            const auto fence_error = m_fence.shutdown();
            m_queue.setNull();
            const auto rhi_error = m_rhi_attempted ? m_rhi->shutdown() : std::error_code{};
            const auto shader_error = m_shaders_attempted ? m_shaders->shutdown() : std::error_code{};
            for (const auto error: {matter_error, pass_error, grid_error, fence_error, rhi_error, shader_error}) {
                if (std::uncaught_exceptions() == 0)
                    PPR_TEST_ASSERT(not error);
                else if (error)
                    std::println("WORLD_RC_CLEANUP error={}", error.message());
            }
        }
    };

    // ------------------------------------------------------------------
    // shared fixed masks, independent cell-ray oracle and actual kernel queries
    // ------------------------------------------------------------------

    struct Shape {
        std::vector<u16> m_ids;
        std::vector<lighting::WorldRadianceMaterial> m_materials;
        std::vector<lighting::OpticalCell> m_cells;
        std::shared_ptr<const lighting::WorldRadianceSource> m_source;
        u32 m_width{}, m_height{};
    };

    Shape shape_(const bool constant, const bool open) {
        const auto fixture = RadianceGpu::backgroundReceivers_(constant, open);
        const auto view = fixture->geometry()->view();
        Shape result;
        result.m_width = view.m_width;
        result.m_height = view.m_height;
        result.m_ids.assign(view.m_labels.begin(), view.m_labels.end());
        for (const auto &row: view.m_materials)
            result.m_materials.push_back({row.m_optics, row.m_material});

        for (const auto id: result.m_ids) {
            const auto row = std::ranges::find(result.m_materials, id, &lighting::WorldRadianceMaterial::m_material_id);
            PPR_TEST_ASSERT(row != result.m_materials.end());
            result.m_cells.push_back(row->m_optics);
        }

        const auto captured = lighting::WorldRadianceSource::capture(
            {result.m_ids, result.m_materials, {}, 1.0, result.m_width, result.m_height,
                lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(captured.has_value());
        result.m_source = *captured;
        return result;
    }

    double directMean_(const Shape &shape, const std::array<double, 2> point, const bool uploaded256 = false) {
        // The 1024-direction physical reference is unchanged. The independent
        // 256-direction slab control promotes the ACTUAL uploaded binary32
        // directions/source values, isolating traversal from angular sampling.
        std::vector<lighting::OpticalCell> uploaded_cells;
        if (uploaded256) {
            uploaded_cells = shape.m_cells;
            for (auto &cell: uploaded_cells) {
                for (auto &channel: cell.m_surface_emission)
                    channel = static_cast<double>(static_cast<float>(channel));
            }
        }

        const lighting::OpticalGridView grid{
            uploaded256 ? std::span<const lighting::OpticalCell>{uploaded_cells}
                        : std::span<const lighting::OpticalCell>{shape.m_cells},
            {}, 1.0, shape.m_width, shape.m_height, lighting::OpticalBoundary::vacuum};
        double result{};
        const u32 bins = uploaded256 ? 256u : 1024u;
        for (u32 bin = 0u; bin < bins; ++bin) {
            const double angle = 2.0 * std::numbers::pi * (double(bin) + 0.5) / static_cast<double>(bins);
            const std::array<double, 2> direction =
                uploaded256
                    ? std::array<
                          double,
                          2>{static_cast<double>(static_cast<float>(std::cos(angle))), static_cast<double>(static_cast<float>(std::sin(angle)))}
                    : std::array<double, 2>{std::cos(angle), std::sin(angle)};
            const auto traced = lighting::traceOpticalReference(grid, {point, direction, 0.0, 64.0});
            PPR_TEST_ASSERT(traced.has_value());
            result += traced->m_radiance[0] / static_cast<double>(bins);
        }

        return result;
    }

    std::array<double, 3> meanAt_(
        const lighting::WorldRadianceGeneration &generation,
        const std::span<const lighting::WorldRadianceCoefficients> rows, const std::array<double, 2> point) {
        const auto minimum = generation.probeMinimum();
        const auto counts = generation.probeCounts();
        const double spacing = static_cast<double>(static_cast<u32>(generation.refinedSpacing()));
        const double x = point[0] / spacing - 0.5, y = point[1] / spacing - 0.5;
        const i32 lx = static_cast<i32>(std::floor(x)), ly = static_cast<i32>(std::floor(y));
        const double fx = x - lx, fy = y - ly;
        std::array<double, 3> value{};
        for (u32 sy = 0u; sy < 2u; ++sy) {
            for (u32 sx = 0u; sx < 2u; ++sx) {
                const double weight = (sx == 0u ? 1.0 - fx : fx) * (sy == 0u ? 1.0 - fy : fy);
                if (weight == 0.0)
                    continue;

                const i32 px = lx + static_cast<i32>(sx) - minimum[0];
                const i32 py = ly + static_cast<i32>(sy) - minimum[1];
                PPR_TEST_ASSERT(
                    px >= 0 and py >= 0 and px < static_cast<i32>(counts[0]) and py < static_cast<i32>(counts[1]));
                const auto &row = rows[u64{static_cast<u32>(py)} * counts[0] + static_cast<u32>(px)];
                PPR_TEST_ASSERT(row.m_status == 0u);
                for (u32 channel = 0u; channel < 3u; ++channel)
                    value[channel] += weight * static_cast<double>(row.m_coefficients[channel]);
            }
        }

        return value;
    }

    // Bounded qualification metadata: supplied moments are analytic constants,
    // never a CPU transport solver. Device rows and all keys use the shared DDA.
    struct OverlayTrace {
        std::span<const lighting::WorldRadianceOverlayCell> m_cells;
        std::span<const u32> m_face_keys;
        std::span<const std::array<float, 3u>> m_moments;
        std::vector<u32> m_discovered;
        std::vector<std::array<float, 3u>> m_baked_moments;
        std::array<u32, 4u> m_append{};
        u32 m_capacity{};
        float m_cell_metres{1.0f};
        std::array<float, 3u> m_sky{};
        bool m_bake_moments{}, m_parallel_moments{};
    };

    std::pair<std::vector<Interval>, std::vector<u32>> traceRows_(
        Fixture &fixture, const Shape &shape, const std::span<const Query> queries, const bool block_hierarchy = false,
        const bool secondary = false, OverlayTrace *const overlay = nullptr,
        const std::shared_ptr<const lighting::WorldRadianceSource> &projected = {}, const bool skip_table = false,
        const u32 sky_hour_plus_one = 0u) {
        PPR_TEST_ASSERT(not projected or overlay != nullptr);

        std::vector<u32> packed((shape.m_ids.size() + 1u) / 2u);
        std::vector<Material> palette(24u);
        for (const auto &row: shape.m_materials) {
            auto &destination = palette[row.m_material_id];
            destination.kind = row.m_optics.m_kind == lighting::OpticalKind::vacuum   ? 0u
                               : row.m_optics.m_kind == lighting::OpticalKind::opaque ? 1u
                                                                                      : 2u;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                destination.source[channel] = static_cast<float>(row.m_optics.m_surface_emission[channel]);
                destination.sigma[channel] = static_cast<float>(row.m_optics.m_extinction[channel]);
                destination.emission[channel] = static_cast<float>(row.m_optics.m_volume_emission[channel]);
                destination.m_albedo[channel] = row.m_diffuse_albedo[channel];
            }
        }

        for (u32 index = 0u; index < shape.m_ids.size(); ++index)
            packed[index / 2u] |= u32{shape.m_ids[index]} << (16u * (index % 2u));

        std::vector<u32> flags(shape.m_ids.size(), 0u);
        std::vector<Hierarchy> hierarchy{{0u, shape.m_width, shape.m_height, 1u}};
        if (block_hierarchy) {
            // Certify complete aligned blocks against the terrain labels. A
            // projected Matter source supplies its production ancestor bitset.
            PPR_TEST_ASSERT(not overlay or projected);
            for (u32 size = 2u; size <= 32u and size <= std::max(shape.m_width, shape.m_height); size *= 2u) {
                const u32 width = (shape.m_width + size - 1u) / size;
                const u32 height = (shape.m_height + size - 1u) / size;
                hierarchy.push_back({safe_narrowing(flags.size()), width, height, size});

                for (u32 y = 0u; y < height; ++y) {
                    for (u32 x = 0u; x < width; ++x) {
                        bool empty = (x + 1u) * size <= shape.m_width and (y + 1u) * size <= shape.m_height;
                        for (u32 dy = 0u; empty and dy < size; ++dy) {
                            for (u32 dx = 0u; empty and dx < size; ++dx)
                                empty = palette[shape.m_ids[(y * size + dy) * shape.m_width + x * size + dx]].kind ==
                                        0u;
                        }
                        flags.push_back(u32{empty});
                    }
                }
            }
        }

        const u32 hierarchy_count = safe_narrowing(hierarchy.size());
        Parameters parameters{
            shape.m_width, shape.m_height, static_cast<u32>(palette.size()), hierarchy_count, {},
            shape.m_width + shape.m_height + 4u};
        parameters.sky_hour_plus_one = sky_hour_plus_one;
        if (sky_hour_plus_one != 0u) {
            // This raw ray fixture supplies the noon direction and intensity.
            PPR_TEST_ASSERT(sky_hour_plus_one == 13u);
            parameters.sky[1] = parameters.sky[2] = 1.0f;
        } else if (overlay) {
            std::ranges::copy(overlay->m_sky, parameters.sky);
        }

        std::vector<u32> overlay_lookup(1u), face_lookup(1u);
        rhi::ComPtr<rhi::IBuffer> overlay_rows, overlay_table, overlay_ancestors, face_keys, face_moments, face_table;
        rhi::ComPtr<rhi::IBuffer> discovery_bits, discovery_keys, discovery_state, moment_directions;
        const u32 face_universe = 4u * shape.m_width * shape.m_height;
        const std::array<u32, 8u> discovery_dispatch{
            0u, 0u, 0u, 0u, 1u, overlay and overlay->m_capacity != 0u ? overlay->m_capacity : face_universe,
            (face_universe + 31u) / 32u, 0u};
        BounceDispatch bounce{};
        if (overlay) {
            if (projected) {
                PPR_TEST_ASSERT(projected->matterIdentity().has_value());
                PPR_TEST_ASSERT(
                    projected->overlayBuffer() and
                    projected->overlayLookupBuffer() and
                    projected->overlayAncestorBuffer());
                parameters.cell_metres = static_cast<float>(projected->cellMetres());
                parameters.overlay_count = projected->overlayCount();
                parameters.overlay_capacity = projected->overlayLookupCapacity();
                parameters.overlay_ancestor_words = projected->overlayAncestorWords();
                overlay_rows = projected->overlayBuffer();
                overlay_table = projected->overlayLookupBuffer();
                overlay_ancestors = projected->overlayAncestorBuffer();
            } else {
                PPR_TEST_ASSERT(not overlay->m_cells.empty());
                while (overlay_lookup.size() < 2u * overlay->m_cells.size())
                    overlay_lookup.resize(2u * overlay_lookup.size());

                for (u32 ordinal = 0u; ordinal < overlay->m_cells.size(); ++ordinal) {
                    u32 slot = (overlay->m_cells[ordinal].m_cell * 2654435761u) &
                               static_cast<u32>(overlay_lookup.size() - 1u);
                    while (overlay_lookup[slot] != 0u)
                        slot = (slot + 1u) & static_cast<u32>(overlay_lookup.size() - 1u);

                    overlay_lookup[slot] = ordinal + 1u;
                }

                parameters.cell_metres = overlay->m_cell_metres;
                parameters.overlay_count = safe_narrowing(overlay->m_cells.size());
                parameters.overlay_capacity = safe_narrowing(overlay_lookup.size());
                overlay_rows = fixture.buffer(
                    overlay->m_cells.size_bytes(), sizeof(lighting::WorldRadianceOverlayCell), overlay->m_cells.data());
                overlay_table = fixture.buffer(overlay_lookup.size() * sizeof(u32), sizeof(u32), overlay_lookup.data());
            }

            discovery_bits = fixture.buffer(u64{discovery_dispatch[6]} * sizeof(u32), sizeof(u32));
            discovery_keys = fixture.buffer(u64{discovery_dispatch[5]} * sizeof(u32), sizeof(u32));
            discovery_state = fixture.buffer(4u * sizeof(u32), sizeof(u32));

            PPR_TEST_ASSERT(overlay->m_face_keys.size() == overlay->m_moments.size());
            if (not overlay->m_face_keys.empty()) {
                bounce.faces = safe_narrowing(overlay->m_face_keys.size());
                while (face_lookup.size() < 2u * overlay->m_face_keys.size())
                    face_lookup.resize(2u * face_lookup.size());

                bounce.capacity = safe_narrowing(face_lookup.size());
                face_keys = fixture.buffer(overlay->m_face_keys.size_bytes(), sizeof(u32), overlay->m_face_keys.data());
                face_moments = fixture.buffer(
                    overlay->m_moments.size_bytes(), 3u * sizeof(float), overlay->m_moments.data());
                face_table = fixture.buffer(face_lookup.size() * sizeof(u32), sizeof(u32), face_lookup.data());
                if (overlay->m_bake_moments) {
                    std::array<std::array<float, 2u>, 256u> directions{};
                    for (u32 bin = 0u; bin < directions.size(); ++bin) {
                        const double angle = 2.0 * std::numbers::pi * (double(bin) + .5) / 256.0;
                        directions[bin] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
                    }

                    moment_directions = fixture.buffer(sizeof(directions), sizeof(directions[0]), directions.data());
                }
            }
        }

        const Dispatch dispatch{secondary ? 1u : 0u, 0u, static_cast<u32>(queries.size()), 0u};
        auto ids = fixture.buffer(packed.size() * sizeof(u32), sizeof(u32), packed.data());
        auto materials = fixture.buffer(palette.size() * sizeof(Material), sizeof(Material), palette.data());
        auto vacuum = fixture.buffer(flags.size() * sizeof(u32), sizeof(u32), flags.data());
        auto metadata = fixture.buffer(hierarchy_count * sizeof(Hierarchy), sizeof(Hierarchy), hierarchy.data());
        auto rays = fixture.buffer(queries.size() * sizeof(Query), sizeof(Query), queries.data());
        auto output = fixture.buffer(queries.size() * sizeof(Interval), sizeof(Interval));
        auto counts = fixture.buffer(8u * sizeof(u32), sizeof(u32));

        const u32 trace_program = skip_table ? 7u : 0u;
        rhi::ComPtr<rhi::IShaderObject> root;
        PPR_TEST_ASSERT(not make_error_code(
            fixture.m_rhi->getDevice().createRootShaderObject(fixture.m_programs[trace_program], root.writeRef())));
        rhi::ShaderCursor cursor{root};
        fixture.constant(cursor, "g_world", &parameters, sizeof(parameters));
        fixture.constant(cursor, "g_dispatch", &dispatch, sizeof(dispatch));
        fixture.bindUnusedBounce(cursor);
        if (overlay) {
            fixture.bind(cursor, "g_overlay", overlay_rows);
            fixture.bind(cursor, "g_overlay_lookup", overlay_table);
            if (overlay_ancestors)
                fixture.bind(cursor, "g_overlay_ancestors", overlay_ancestors);
            fixture.constant(cursor, "g_discovery", discovery_dispatch.data(), sizeof(discovery_dispatch));
            fixture.bind(cursor, "g_discovery_bits", discovery_bits);
            fixture.bind(cursor, "g_discovery_keys", discovery_keys);
            fixture.bind(cursor, "g_discovery_state", discovery_state);
            if (bounce.faces != 0u) {
                fixture.constant(cursor, "g_bounce", &bounce, sizeof(bounce));
                fixture.bind(cursor, "g_face_keys", face_keys);
                fixture.bind(cursor, "g_face_lookup", face_table);
                fixture.bind(cursor, "g_face_moments", face_moments);
            }
        }

        for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 7>{
                 std::pair{"g_cell_ids", ids.get()}, {"g_materials", materials.get()}, {"g_vacuum", vacuum.get()},
                 {"g_hierarchy", metadata.get()}, {"g_queries", rays.get()}, {"g_current", output.get()},
                 {"g_counts", counts.get()}})
            fixture.bind(cursor, name, buffer);

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        for (auto *const buffer: {ids.get(), materials.get(), vacuum.get(), metadata.get(), rays.get()})
            encoder->setBufferState(buffer, rhi::ResourceState::ShaderResource);

        if (overlay) {
            encoder->setBufferState(overlay_rows, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(overlay_table, rhi::ResourceState::ShaderResource);
            if (overlay_ancestors)
                encoder->setBufferState(overlay_ancestors, rhi::ResourceState::ShaderResource);
            for (auto *const buffer: {discovery_bits.get(), discovery_state.get()}) {
                encoder->setBufferState(buffer, rhi::ResourceState::CopyDestination);
                encoder->clearBuffer(buffer, 0u, buffer->getDesc().size);
                encoder->setBufferState(buffer, rhi::ResourceState::UnorderedAccess);
            }

            encoder->setBufferState(discovery_keys, rhi::ResourceState::UnorderedAccess);
            if (bounce.faces != 0u) {
                encoder->setBufferState(face_keys, rhi::ResourceState::ShaderResource);
                encoder->setBufferState(face_table, rhi::ResourceState::UnorderedAccess);
                encoder->setBufferState(face_moments, rhi::ResourceState::UnorderedAccess);
                if (moment_directions)
                    encoder->setBufferState(moment_directions, rhi::ResourceState::ShaderResource);
            }
        }

        encoder->setBufferState(fixture.m_unused_face_keys, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(fixture.m_unused_face_lookup, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(fixture.m_unused_face_moments, rhi::ResourceState::UnorderedAccess);

        encoder->setBufferState(counts, rhi::ResourceState::CopyDestination);
        encoder->clearBuffer(counts, 0u, 8u * sizeof(u32));
        encoder->setBufferState(counts, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);

        rhi::ComPtr<rhi::IShaderObject> lookup_root;
        if (bounce.faces != 0u) {
            PPR_TEST_ASSERT(not make_error_code(
                fixture.m_rhi->getDevice().createRootShaderObject(fixture.m_programs[3], lookup_root.writeRef())));
            rhi::ShaderCursor lookup_cursor{lookup_root};
            const Dispatch build{0u, 0u, bounce.faces, 0u};
            fixture.constant(lookup_cursor, "g_world", &parameters, sizeof(parameters));
            fixture.constant(lookup_cursor, "g_dispatch", &build, sizeof(build));
            fixture.constant(lookup_cursor, "g_bounce", &bounce, sizeof(bounce));
            if (overlay_ancestors)
                fixture.bind(lookup_cursor, "g_overlay_ancestors", overlay_ancestors);

            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 7u>{
                     std::pair{"g_overlay", overlay_rows.get()}, {"g_overlay_lookup", overlay_table.get()},
                     {"g_cell_ids", ids.get()}, {"g_materials", materials.get()}, {"g_face_keys", face_keys.get()},
                     {"g_face_lookup", face_table.get()}, {"g_counts", counts.get()}})
                fixture.bind(lookup_cursor, name, buffer);

            auto *const build_pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(build_pass != nullptr);
            build_pass->bindPipeline(fixture.m_pipelines[3], lookup_root);
            build_pass->dispatchCompute((bounce.faces + 63u) / 64u, 1u, 1u);
            build_pass->end();
            encoder->globalBarrier();
        }

        rhi::ComPtr<rhi::ITexture> table;
        rhi::ComPtr<rhi::ITextureView> table_view;
        rhi::ComPtr<rhi::IShaderObject> table_root;
        if (skip_table) {
            auto &device = fixture.m_rhi->getDevice();
            rhi::TextureDesc description{};
            description.type = rhi::TextureType::Texture2D;
            description.size = {shape.m_width, shape.m_height, 1u};
            description.format = rhi::Format::R8Uint;
            description.usage = static_cast<rhi::TextureUsage>(
                u32(rhi::TextureUsage::ShaderResource) | u32(rhi::TextureUsage::UnorderedAccess));
            description.defaultState = rhi::ResourceState::ShaderResource;
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(description, nullptr, table.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(table->getDefaultView(table_view.writeRef())));

            PPR_TEST_ASSERT(
                not make_error_code(device.createRootShaderObject(fixture.m_programs[6], table_root.writeRef())));
            rhi::ShaderCursor build{table_root};
            fixture.constant(build, "g_world", &parameters, sizeof(parameters));
            fixture.bind(build, "g_cell_ids", ids);
            fixture.bind(build, "g_materials", materials);
            fixture.bind(build, "g_vacuum", vacuum);
            fixture.bind(build, "g_hierarchy", metadata);
            fixture.bind(build, "g_overlay", overlay ? overlay_rows.get() : fixture.m_unused_overlay.get());
            fixture.bind(
                build, "g_overlay_lookup", overlay ? overlay_table.get() : fixture.m_unused_overlay_lookup.get());
            fixture.bind(
                build, "g_overlay_ancestors",
                overlay_ancestors ? overlay_ancestors.get() : fixture.m_unused_overlay_lookup.get());
            PPR_TEST_ASSERT(not make_error_code(build["g_skip_table_output"].setBinding(rhi::Binding(table_view))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_skip_table"].setBinding(rhi::Binding(table_view))));

            const rhi::SubresourceRange range{0u, 1u, 0u, 1u};
            encoder->setTextureState(table, range, rhi::ResourceState::UnorderedAccess);
            auto *const build_pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(build_pass != nullptr);
            build_pass->bindPipeline(fixture.m_pipelines[6], table_root);
            build_pass->dispatchCompute((shape.m_width + 7u) / 8u, (shape.m_height + 7u) / 8u, 1u);
            build_pass->end();
            encoder->globalBarrier();
            encoder->setTextureState(table, range, rhi::ResourceState::ShaderResource);
        }

        rhi::ComPtr<rhi::IShaderObject> moment_root;
        if (moment_directions) {
            PPR_TEST_ASSERT(not make_error_code(fixture.m_rhi->getDevice().createRootShaderObject(
                fixture.m_programs[overlay->m_parallel_moments ? 5u : 4u], moment_root.writeRef())));
            rhi::ShaderCursor moment_cursor{moment_root};
            const Dispatch bake{0u, 0u, bounce.faces, 0u};
            fixture.constant(moment_cursor, "g_world", &parameters, sizeof(parameters));
            fixture.constant(moment_cursor, "g_dispatch", &bake, sizeof(bake));
            fixture.constant(moment_cursor, "g_bounce", &bounce, sizeof(bounce));
            if (overlay_ancestors)
                fixture.bind(moment_cursor, "g_overlay_ancestors", overlay_ancestors);

            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 11u>{
                     std::pair{"g_overlay", overlay_rows.get()}, {"g_overlay_lookup", overlay_table.get()},
                     {"g_cell_ids", ids.get()}, {"g_materials", materials.get()}, {"g_vacuum", vacuum.get()},
                     {"g_hierarchy", metadata.get()}, {"g_face_keys", face_keys.get()},
                     {"g_face_lookup", face_table.get()}, {"g_face_moments", face_moments.get()},
                     {"g_directions", moment_directions.get()}, {"g_counts", counts.get()}})
                fixture.bind(moment_cursor, name, buffer);

            auto *const bake_pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(bake_pass != nullptr);
            bake_pass->bindPipeline(fixture.m_pipelines[overlay->m_parallel_moments ? 5u : 4u], moment_root);
            bake_pass->dispatchCompute(overlay->m_parallel_moments ? bounce.faces : (bounce.faces + 63u) / 64u, 1u, 1u);
            bake_pass->end();
            encoder->globalBarrier();
        }

        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);

        pass->bindPipeline(fixture.m_pipelines[trace_program], root);
        pass->dispatchCompute((static_cast<u32>(queries.size()) + 63u) / 64u, 1u, 1u);
        pass->end();
        encoder->globalBarrier();

        fixture.submit(*encoder, false);

        if (overlay) {
            const auto state = fixture.read<u32>(discovery_state);
            std::ranges::copy(state, overlay->m_append.begin());
            auto keys = fixture.read<u32>(discovery_keys);
            keys.resize(std::min(state[0], discovery_dispatch[5]));
            overlay->m_discovered = std::move(keys);
            if (moment_directions)
                overlay->m_baked_moments = fixture.read<std::array<float, 3u>>(face_moments);
        }

        return {fixture.read<Interval>(output), fixture.read<u32>(counts)};
    }

    auto checkedSkipRows_(
        Fixture &fixture, const Shape &shape, const std::span<const Query> queries, const bool blocks = false,
        OverlayTrace *const overlay = nullptr,
        const std::shared_ptr<const lighting::WorldRadianceSource> &projected = {}) {
        const auto hierarchy = traceRows_(fixture, shape, queries, blocks, false, overlay, projected);
        auto table = traceRows_(fixture, shape, queries, blocks, false, overlay, projected, true);
        PPR_TEST_ASSERT(table.second == hierarchy.second and table.first.size() == hierarchy.first.size());
        PPR_TEST_ASSERT(
            std::memcmp(table.first.data(), hierarchy.first.data(), table.first.size() * sizeof(Interval)) == 0);
        return table;
    }

    void checkRebuiltSkipRays_(Fixture &fixture) {
        Shape shape{};
        shape.m_width = 64u;
        shape.m_height = 32u;
        shape.m_materials = {
            {{{}, {}, {}, lighting::OpticalKind::vacuum}, 0u},
            {{{}, {}, {1.0, 2.0, 3.0}, lighting::OpticalKind::opaque}, 1u}};
        shape.m_ids.resize(shape.m_width * shape.m_height);

        const std::array<Query, 4u> rays{
            {{0.5f, 15.5f, 1, 0, 0, 63}, {0.5f, 15.5f, 1, 0, 0, 63, 0u, 1u}, {63.5f, 15.5f, -1, 0, 0, 63},
                {0.5f, 15.5f, 1, 0, 0, 31.5f}}};
        for (const u32 wall_x: {32u, 48u, shape.m_width}) {
            std::ranges::fill(shape.m_ids, 0u);
            if (wall_x < shape.m_width)
                shape.m_ids[15u * shape.m_width + wall_x] = 1u;

            const auto [rows, counts] = checkedSkipRows_(fixture, shape, rays, true);
            PPR_TEST_ASSERT(counts[1] == rays.size() and counts[2] == 0u and counts[6] > 0u);
            PPR_TEST_ASSERT(rows[3].transmission[0] == 1.0f);
            if (wall_x == shape.m_width) {
                PPR_TEST_ASSERT(rows[0].transmission[0] == 1.0f and rows[1].segments == 0xffffffffu);
            } else {
                // One 32-cell jump reaches the first wall. Its moved position
                // needs one additional 16-cell jump before the hit segment.
                PPR_TEST_ASSERT(rows[0].segments == (wall_x == 32u ? 2u : 3u));
                PPR_TEST_ASSERT(rows[0].source[2] == 3.0f and rows[2].source[2] == 3.0f);
                PPR_TEST_ASSERT(rows[1].segments == 4u * (15u * shape.m_width + wall_x));
            }
        }
    }

    void traceChecks_(Fixture &fixture, const Shape &shape, const bool initial_face_case = false) {
        std::vector<Query> queries{
            {3.5f, 5.5f, 1.0f, 0.0f, 0.0f, 32.0f}, {7.5f, 5.5f, -1.0f, 0.0f, 0.0f, 32.0f},
            {6.0f, 4.0f, -1.0f, 0.0f, 0.0f, 32.0f}, {6.0f, 4.0f, 1.0f, 0.0f, 0.0f, 32.0f},
            {7.0f, 7.0f, -1.0f, -1.0f, 0.0f, 32.0f}, {-2.0f, 4.5f, 1.0f, 0.0f, 0.0f, 32.0f},
            {12.0f, 4.5f, 1.0f, 0.0f, 0.0f, 32.0f}, {7.5f, 4.5f, 0.0f, -1.0f, 0.0f, 0.0f},
            {-2.0f, 4.5f, 1.0f, 0.0f, 0.0f, 1.0f, 1u}};
        std::optional<u32> ownership_index;
        if (initial_face_case) {
            // Same binary32 direction construction as the producer's C1/bin1
            // table, for this declared phase. The exact parametric near point
            // is BELOW y=4 although origin + direction*near rounds to y=4.
            constexpr float phase = 0.33333325f;
            const double angle = 2.0 * std::numbers::pi * (1.0 + double(phase)) / 16.0;
            const float dx = static_cast<float>(std::cos(angle)), dy = static_cast<float>(std::sin(angle));
            PPR_TEST_ASSERT(std::bit_cast<u32>(dy) == 0x3effffffu);
            PPR_TEST_ASSERT(2.0 + 4.0 * double(dy) < 4.0);
            PPR_TEST_ASSERT(2.0 + 4.0 * double(dx) > 5.0 and 2.0 + 4.0 * double(dx) < 6.0);
            ownership_index = static_cast<u32>(queries.size()) - 1u;
            queries.insert(queries.end() - 1, {2.0f, 2.0f, dx, dy, 4.0f, 32.0f});
        }

        const auto [actual, aggregate] = checkedSkipRows_(fixture, shape, queries);
        PPR_TEST_ASSERT(aggregate[0] == queries.size() and aggregate[1] + 1u == queries.size() and aggregate[2] == 1u);
        PPR_TEST_ASSERT(aggregate[3] == queries.size() and aggregate[4] == 1u);
        PPR_TEST_ASSERT(actual.back().status == 2u); // Too-short terminal is visible incomplete work, never sky.

        const lighting::OpticalGridView grid{
            shape.m_cells, {}, 1.0, shape.m_width, shape.m_height, lighting::OpticalBoundary::vacuum};
        for (u32 index = 0u; index + 1u < queries.size(); ++index) {
            const auto &query = queries[index];
            if (ownership_index and index == *ownership_index) {
                // Independent slab fact: the uploaded near point is strictly
                // inside sole opaque cell(5,3), so emission precedes any step.
                PPR_TEST_ASSERT(actual[index].status == 0u);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const auto material = std::ranges::find(
                        shape.m_materials, 23u, &lighting::WorldRadianceMaterial::m_material_id);
                    PPR_TEST_ASSERT(material != shape.m_materials.end());
                    PPR_TEST_ASSERT(
                        actual[index].source[channel] ==
                        static_cast<float>(material->m_optics.m_surface_emission[channel]));
                    PPR_TEST_ASSERT(actual[index].transmission[channel] == 0.0f);
                }
                continue;
            }

            const auto expected = lighting::traceOpticalReference(
                grid, {{static_cast<double>(query.x), static_cast<double>(query.y)},
                          {static_cast<double>(query.dx), static_cast<double>(query.dy)},
                          static_cast<double>(query.near_cells), static_cast<double>(query.far_cells)});
            PPR_TEST_ASSERT(expected.has_value() and actual[index].status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(
                    std::abs(double(actual[index].source[channel]) - expected->m_radiance[channel]) <= 1e-6);
                PPR_TEST_ASSERT(
                    std::abs(double(actual[index].transmission[channel]) - expected->m_transmittance[channel]) <= 1e-6);
            }
        }
    }

    // ------------------------------------------------------------------
    // actual native entered-face ownership and complete contributor discovery
    // ------------------------------------------------------------------

    void enteredFaceChecks_(Fixture &fixture) {
        Shape shape{};
        shape.m_width = 88u;
        shape.m_height = 8u;
        shape.m_ids.resize(shape.m_width * shape.m_height);
        shape.m_materials = {
            {{{}, {}, {}, lighting::OpticalKind::vacuum}, 0u},
            {{{}, {}, {.25, .5, 1.0}, lighting::OpticalKind::opaque}, 1u},
            {{{}, {}, {2.0, 3.0, 4.0}, lighting::OpticalKind::opaque}, 2u},
            {{{}, {}, {5.0, 6.0, 7.0}, lighting::OpticalKind::opaque}, 3u}};
        const auto paint = [&](const u32 tile, const u32 x, const u32 y, const u16 id) {
            shape.m_ids[y * shape.m_width + tile * 8u + x] = id;
        };

        std::array<Query, 11u> rays{
            {{-1, 3.5f, 1, 0, 0, 2}, {2.5f, 2.5f, 1, 1, 0, 2}, {2, 2.5f, 2, 1, 0, 1}, {2.5f, 2.5f, 1, 1, 0, 2},
                {4.5f, 4.5f, -1, -1, 0, 2}, {2.5f, 2.5f, 1, 1, 0, 1}, {3.5f, 3.5f, 1, 0, 0, 1}, {2.5f, 2, 1, 2, 0, 1},
                {3.5f, 1.5f, 0, 1, 0, 3}, {5.5f, 3.5f, -1, 0, 0, 3}, {3.5f, 5.5f, 0, -1, 0, 3}}};
        const std::array<std::array<u32, 3u>, 11u> expected{
            {{0, 3, 0}, {3, 3, 0}, {3, 3, 2}, {3, 2, 0}, {3, 4, 1}, {0, 0, 0xffffffffu}, {0, 0, 0xffffffffu}, {3, 3, 2},
                {3, 3, 2}, {3, 3, 1}, {3, 3, 3}}};
        for (u32 tile = 0u; tile < rays.size(); ++tile) {
            if (tile != 5u) {
                paint(tile, tile == 0u ? 0u : 3u, 3u, 1u);
            }
        }

        paint(2u, 2u, 3u, 2u); // Sole exposed Y face, despite dominant X direction.

        paint(3u, 2u, 3u, 2u);
        paint(3u, 3u, 2u, 3u); // Neither hit-cell face exposed: axial owner differs.

        paint(4u, 4u, 3u, 2u);
        paint(4u, 3u, 4u, 3u);

        paint(5u, 3u, 2u, 2u);
        paint(5u, 2u, 3u, 3u); // Diagonal-vacuum grazing must remain NO direct hit.

        std::vector<Query> queries;
        for (u32 tile = 0u; tile < rays.size(); ++tile) {
            auto query = rays[tile];
            query.x += static_cast<float>(tile * 8u);
            queries.push_back(query);
            query.reserved = 1u;
            queries.push_back(query);
        }

        const auto [rows, counts] = checkedSkipRows_(fixture, shape, queries);
        PPR_TEST_ASSERT(counts[0] == queries.size() and counts[1] == queries.size() and counts[2] == 0u);
        for (u32 tile = 0u; tile < rays.size(); ++tile) {
            const auto &direct = rows[2u * tile];
            const auto &face = rows[2u * tile + 1u];
            PPR_TEST_ASSERT(direct.status == 0u and face.status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::bit_cast<u32>(direct.source[channel]) == std::bit_cast<u32>(face.source[channel]));
                PPR_TEST_ASSERT(
                    std::bit_cast<u32>(direct.transmission[channel]) == std::bit_cast<u32>(face.transmission[channel]));
                PPR_TEST_ASSERT(direct.source[channel] == (tile == 5u ? 0.0f : float(.25 * (1u << channel))));
                PPR_TEST_ASSERT(direct.transmission[channel] == (tile == 5u ? 1.0f : 0.0f));
            }

            if (expected[tile][2] == 0xffffffffu) {
                PPR_TEST_ASSERT(face.segments == 0xffffffffu);
                continue;
            }

            const u32 cell = face.segments >> 2u;
            PPR_TEST_ASSERT(cell % shape.m_width == tile * 8u + expected[tile][0]);
            PPR_TEST_ASSERT(cell / shape.m_width == expected[tile][1]);
            PPR_TEST_ASSERT((face.segments & 3u) == expected[tile][2]);
        }

        // The hierarchy exits X at t=.25 while simultaneously crossing the
        // native Y plane. Both exposed hit faces exist; dominant Y must win.
        Shape skipped{};
        skipped.m_width = 4u;
        skipped.m_height = 2u;
        skipped.m_ids.resize(8u);
        skipped.m_ids[6u] = 1u;
        skipped.m_materials = shape.m_materials;

        const std::array<Query, 2u> skipped_rays{{{1.75f, .5f, 1, 2, 0, .5f}, {1.75f, .5f, 1, 2, 0, .5f, 0u, 1u}}};
        const auto [skipped_rows, skipped_counts] = checkedSkipRows_(fixture, skipped, skipped_rays, true);
        PPR_TEST_ASSERT(skipped_counts[0] == 2u and skipped_counts[1] == 2u and skipped_counts[2] == 0u);
        PPR_TEST_ASSERT(skipped_counts[6] > 0u);
        PPR_TEST_ASSERT(skipped_rows[0].status == 0u and skipped_rows[1].status == 0u);
        PPR_TEST_ASSERT(skipped_rows[1].segments == (6u << 2u | 2u));

        for (u32 channel = 0u; channel < 3u; ++channel) {
            PPR_TEST_ASSERT(skipped_rows[0].source[channel] == float(.25 * (1u << channel)));
            PPR_TEST_ASSERT(skipped_rows[0].transmission[channel] == 0.0f);
            PPR_TEST_ASSERT(
                std::bit_cast<u32>(skipped_rows[0].source[channel]) ==
                std::bit_cast<u32>(skipped_rows[1].source[channel]));
            PPR_TEST_ASSERT(
                std::bit_cast<u32>(skipped_rows[0].transmission[channel]) ==
                std::bit_cast<u32>(skipped_rows[1].transmission[channel]));
        }

        // Native X=2 and Y=1 event quotients tie, although reconstruction is
        // one float below Y=1. Unit-cell ownership must determine the entered
        // cell before BOTH direct material lookup and discovered face selection.
        constexpr float dx = 20.5f / 64.0f, dy = 41.0f / 64.0f;
        const float x_event = .5f / dx, y_event = 1.0f / dy;
        PPR_TEST_ASSERT(x_event == y_event);
        PPR_TEST_ASSERT(std::bit_cast<u32>(dy * x_event) == 0x3f7fffffu);

        const std::array<Query, 2u> rounded_rays{{{1.5f, 0, dx, dy, 0, 2}, {1.5f, 0, dx, dy, 0, 2, 0u, 1u}}};
        const auto check_native_agreement = [&](const u32 expected_key, const std::array<float, 3u> source,
                                                const float transmission) {
            const auto [native_rows, native_counts] = traceRows_(fixture, skipped, rounded_rays);
            const auto [hierarchy_rows, hierarchy_counts] = checkedSkipRows_(fixture, skipped, rounded_rays, true);
            PPR_TEST_ASSERT(native_counts[0] == 2u and native_counts[1] == 2u and native_counts[2] == 0u);
            PPR_TEST_ASSERT(hierarchy_counts[0] == 2u and hierarchy_counts[1] == 2u and hierarchy_counts[2] == 0u);
            PPR_TEST_ASSERT(native_counts[6] == 0u and hierarchy_counts[6] > 0u);
            PPR_TEST_ASSERT(native_rows[1].segments == expected_key);
            PPR_TEST_ASSERT(hierarchy_rows[1].segments == native_rows[1].segments);

            for (u32 row = 0u; row < rounded_rays.size(); ++row) {
                PPR_TEST_ASSERT(native_rows[row].status == 0u and hierarchy_rows[row].status == 0u);

                for (u32 channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT(native_rows[row].source[channel] == source[channel]);
                    PPR_TEST_ASSERT(native_rows[row].transmission[channel] == transmission);
                    PPR_TEST_ASSERT(
                        std::bit_cast<u32>(native_rows[row].source[channel]) ==
                        std::bit_cast<u32>(hierarchy_rows[row].source[channel]));
                    PPR_TEST_ASSERT(
                        std::bit_cast<u32>(native_rows[row].transmission[channel]) ==
                        std::bit_cast<u32>(hierarchy_rows[row].transmission[channel]));
                }
            }
        };

        skipped.m_ids[2u] = 1u;
        check_native_agreement(6u << 2u, {.25f, .5f, 1.0f}, 0.0f);

        skipped.m_ids[2u] = 2u;
        skipped.m_ids[6u] = 3u;
        check_native_agreement(6u << 2u, {5.0f, 6.0f, 7.0f}, 0.0f);

        skipped.m_ids[6u] = 0u;
        check_native_agreement(0xffffffffu, {}, 1.0f); // Opaque axial grazing is NOT a diagonal hit.
    }

    void discoveryChecks_(Fixture &fixture) {
        // Prepare required immutable GPU work before submitting the direct donor.
        // Repeating preparation must preserve all existing donor/fence contracts.
        PPR_TEST_ASSERT(not fixture.m_pass.prepareDiscoveryPipeline());
        PPR_TEST_ASSERT(not fixture.m_pass.prepareDiscoveryPipeline());

        std::error_code foreign_owner_error;
        {
            std::jthread foreign_owner{[&] { foreign_owner_error = fixture.m_pass.prepareDiscoveryPipeline(); }};
        }

        PPR_TEST_ASSERT(foreign_owner_error == std::errc::operation_not_permitted);

        std::vector<u16> ids(64u * 32u);
        for (u32 y = 0u; y < 32u; ++y) {
            ids[y * 64u + 48u] = 1u; // Offscreen from the first32x32 receiver window.
        }

        const std::array<lighting::WorldRadianceMaterial, 2u> palette{
            {{{{}, {}, {}, lighting::OpticalKind::vacuum}, 0u},
                {{{}, {}, {1.0, .5, .25}, lighting::OpticalKind::opaque}, 1u}}};
        const auto source = lighting::WorldRadianceSource::capture(
            {ids, palette, {}, 1.0, 64u, 32u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(source.has_value());
        const auto field = fixture.generate(
            *source, 601u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {{0, 0}, {32, 32}});
        const auto field_before = fixture.read<lighting::WorldRadianceCoefficients>(field->coefficientsBuffer());

        lighting::WorldRadianceDiscoverySettings settings{
            64ull * 1024u * 1024u, 1024ull * 1024u, 32ull * 1024u * 1024u, 501u, {{0, 0}, {32, 32}}, 0u, false, true};
        u64 frame = 0u;
        const auto run = [&](const bool overflow) -> std::shared_ptr<const lighting::WorldRadianceFaceDiscovery> {
            const auto demand = fixture.m_pass.prepareDiscovery(
                *source, field->cohort(), settings, fixture.m_fence.completed());
            PPR_TEST_ASSERT(demand.has_value() and not demand->m_admission_error);
            PPR_TEST_ASSERT(demand->m_key_capacity <= demand->m_trace_upper_bound);

            for (u32 batch = 0u; batch < demand->m_batches; ++batch) {
                PPR_TEST_ASSERT(fixture.m_pass.canEncodeDiscovery());

                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));

                PPR_TEST_ASSERT(not fixture.m_pass.encodeDiscovery(
                    {fixture.m_rhi->getDevice(), *encoder, fixture.m_fence.completed()}, frame++));

                fixture.submit(*encoder, true, false);

                const auto pending_cancel = fixture.m_pass.cancelDiscovery(kFenceValueNone);
                PPR_TEST_ASSERT(pending_cancel.has_value() and not *pending_cancel);

                PPR_TEST_ASSERT(not fixture.m_fence.waitFor(fixture.m_fence.lastSubmitted()));

                const auto ready = fixture.m_pass.advanceDiscovery(fixture.m_fence.completed());
                if (not ready) {
                    PPR_TEST_ASSERT(overflow and ready.error() == std::errc::result_out_of_range);

                    const auto canceled = fixture.m_pass.cancelDiscovery(fixture.m_fence.completed());
                    PPR_TEST_ASSERT(canceled.has_value() and *canceled);
                    return {};
                }

                PPR_TEST_ASSERT(*ready == (batch + 1u == demand->m_batches));
                if (not *ready) {
                    rhi::ComPtr<rhi::ICommandEncoder> duplicate;
                    PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(duplicate.writeRef())));
                    PPR_TEST_ASSERT(
                        fixture.m_pass.encodeDiscovery(
                            {fixture.m_rhi->getDevice(), *duplicate, fixture.m_fence.completed()}, frame - 1u) ==
                        std::errc::operation_not_permitted);
                }
            }

            PPR_TEST_ASSERT(not overflow);

            const auto result = fixture.m_pass.acquireDiscovery(fixture.m_fence.completed());
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT((*result)->statistics().m_queries_attempted == demand->m_trace_upper_bound);
            PPR_TEST_ASSERT((*result)->statistics().m_queries_completed == demand->m_trace_upper_bound);
            PPR_TEST_ASSERT((*result)->statistics().m_queries_failed == 0u);
            PPR_TEST_ASSERT((*result)->statistics().m_rows_written == demand->m_record_count);
            return *result;
        };

        const auto a = run(false);
        const auto keys_a = fixture.m_pass.readDiscoveryKeys(fixture.m_fence.completed());
        PPR_TEST_ASSERT(keys_a.has_value() and not keys_a->empty() and keys_a->size() == a->faceCount());

        auto unique = *keys_a;
        std::ranges::sort(unique);

        PPR_TEST_ASSERT(std::ranges::adjacent_find(unique) == unique.end());
        PPR_TEST_ASSERT(std::ranges::all_of(unique, [](const u32 key) { return (key >> 2u) % 64u == 48u; }));
        PPR_TEST_ASSERT(a->source().get() == source->get() and a->requestEpoch() == 501u);
        PPR_TEST_ASSERT(a->completionFence() != kFenceValueNone);

        PPR_TEST_ASSERT(a->faceHitOccurrences() >= a->faceCount());
        PPR_TEST_ASSERT(a->conservativeOffscreenFaceCount() == a->faceCount());

        const auto canceled_a = fixture.m_pass.cancelDiscovery(fixture.m_fence.completed());
        PPR_TEST_ASSERT(canceled_a.has_value() and *canceled_a);

        const auto foreign = lighting::WorldRadianceSource::capture(
            {ids, palette, {}, 1.0, 64u, 32u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(foreign.has_value());
        const auto rejected = fixture.m_pass.prepareDiscovery(
            *foreign, field->cohort(), settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not rejected);

        settings.m_request_epoch = 502u;
        settings.m_window = {{32, 0}, {32, 32}};
        const auto b = run(false);
        PPR_TEST_ASSERT(b->requestEpoch() == 502u and b->keysBuffer() != a->keysBuffer());
        PPR_TEST_ASSERT(b->window().m_minimum[0] == 32 and a->window().m_minimum[0] == 0);

        PPR_TEST_ASSERT(b->faceHitOccurrences() >= b->faceCount());
        PPR_TEST_ASSERT(b->conservativeOffscreenFaceCount() == 0u);

        const auto retained_a = fixture.read<u32>(a->keysBuffer());
        PPR_TEST_ASSERT(std::ranges::equal(*keys_a, std::span{retained_a}.first(keys_a->size())));

        const auto canceled_b = fixture.m_pass.cancelDiscovery(fixture.m_fence.completed());
        PPR_TEST_ASSERT(canceled_b.has_value() and *canceled_b);

        settings.m_request_epoch = 503u;
        settings.m_key_capacity_limit = 1u;
        PPR_TEST_ASSERT(not run(true));

        // A stays valid while a replacement B donor is accepted and then
        // abandoned without acquisition. A changed discovery request restores
        // A by normal prepare/submit/advance/acquire, never by publishing B.
        lighting::WorldRadianceSettings donor_settings{};
        donor_settings.m_budget_bytes = 64ull * 1024u * 1024u;
        donor_settings.m_reserve_bytes = 1024ull * 1024u;
        donor_settings.m_max_resource_bytes = 32ull * 1024u * 1024u;
        donor_settings.m_refined_spacing = lighting::WorldRadianceRefinedSpacing::native_cell_centres;
        donor_settings.m_transport = lighting::WorldRadianceTransport::direct_window;
        donor_settings.m_maximum_response = 65504.0f;
        donor_settings.m_receiver_window = {{0, 0}, {32u, 32u}};
        const lighting::TerrainReceiverCohort canceled_cohort{7u, 11u, 13u, 602u};
        const auto canceled_donor = fixture.m_pass.prepare(
            *foreign, canceled_cohort, donor_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(canceled_donor.has_value() and not canceled_donor->m_admission_error);

        rhi::ComPtr<rhi::ICommandEncoder> canceled_encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(canceled_encoder.writeRef())));

        PPR_TEST_ASSERT(
            not fixture.m_pass.encode({fixture.m_rhi->getDevice(), *canceled_encoder, fixture.m_fence.completed()}));

        fixture.submit(*canceled_encoder, true, false);

        const auto premature_restore = fixture.m_pass.prepare(
            *source, field->cohort(), donor_settings, kFenceValueNone);
        PPR_TEST_ASSERT(not premature_restore and premature_restore.error() == std::errc::device_or_resource_busy);

        PPR_TEST_ASSERT(not fixture.m_fence.waitFor(fixture.m_fence.lastSubmitted()));
        const auto canceled_complete = fixture.m_pass.advance(fixture.m_fence.completed());
        PPR_TEST_ASSERT(canceled_complete.has_value());
        canceled_encoder.setNull();

        const u32 remaining_strips = fixture.m_pass.pendingBatches();
        PPR_TEST_ASSERT(*canceled_complete == (remaining_strips == 0u));

        for (u32 index = 0u; index < remaining_strips; ++index) {
            PPR_TEST_ASSERT(fixture.m_pass.canEncodeNextBatch());
            rhi::ComPtr<rhi::ICommandEncoder> strip_encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(strip_encoder.writeRef())));
            PPR_TEST_ASSERT(
                not fixture.m_pass.encode({fixture.m_rhi->getDevice(), *strip_encoder, fixture.m_fence.completed()}));

            fixture.submit(*strip_encoder, true);
            const auto drained = fixture.m_pass.advance(fixture.m_fence.completed());
            PPR_TEST_ASSERT(drained.has_value() and *drained == (index + 1u == remaining_strips));
        }

        PPR_TEST_ASSERT(fixture.m_pass.pendingBatches() == 0u);
        settings.m_request_epoch = 504u;
        const auto unpublished = fixture.m_pass.prepareDiscovery(
            *foreign, canceled_cohort, settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not unpublished and unpublished.error() == std::errc::device_or_resource_busy);

        const auto restored = fixture.generate(
            *source, 601u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {{0, 0}, {32u, 32u}});
        PPR_TEST_ASSERT(restored->source().get() == source->get() and restored->cohort() == field->cohort());
        const auto restored_rows = fixture.read<lighting::WorldRadianceCoefficients>(restored->coefficientsBuffer());
        PPR_TEST_ASSERT(restored_rows.size() == field_before.size());
        PPR_TEST_ASSERT(
            std::memcmp(restored_rows.data(), field_before.data(), field_before.size() * sizeof(field_before[0])) == 0);

        settings.m_request_epoch = 505u;
        settings.m_key_capacity_limit = 0u;
        settings.m_window = {{0, 0}, {32u, 32u}};

        const auto rediscovered = run(false);
        PPR_TEST_ASSERT(rediscovered->source().get() == source->get() and rediscovered->requestEpoch() == 505u);

        const auto retired_restore = fixture.m_pass.cancelDiscovery(fixture.m_fence.completed());
        PPR_TEST_ASSERT(retired_restore.has_value() and *retired_restore);

        const auto field_after = fixture.read<lighting::WorldRadianceCoefficients>(field->coefficientsBuffer());
        PPR_TEST_ASSERT(
            std::memcmp(field_before.data(), field_after.data(), field_before.size() * sizeof(field_before[0])) == 0);
        std::println(
            "WORLD_DISCOVERY entered_faces=checked offscreen=checked dedup=checked cancel_fence=checked old_result=checked overflow=checked unchanged_direct=checked");
        std::println(
            "WORLD_DISCOVERY_DONOR_RESTORE unpublished_B=retired normal_A=restored retained_A=unchanged discovery_A=complete");
    }

    // ------------------------------------------------------------------
    // shared signed-CH and material-response controls
    // ------------------------------------------------------------------

    void normalChecks_(Fixture &fixture, const std::span<const lighting::WorldRadianceCoefficients> unit_rows) {
        std::vector<NormalQuery> queries{{0.0f, 0.0f, 1.0f, 0u}};
        for (const float z: {0.0f, 0.25f, 0.5f, 0.75f}) {
            const float radius = std::sqrt(1.0f - z * z);
            for (u32 index = 0u; index < 16u; ++index) {
                const double angle = 2.0 * std::numbers::pi * (double(index) + double(index % 3u) / 3.0) / 16.0;
                queries.push_back(
                    {radius * static_cast<float>(std::cos(angle)), radius * static_cast<float>(std::sin(angle)), z,
                        0u});
            }
        }
        const u32 constant_count = static_cast<u32>(queries.size());
        queries.push_back({-1.0f, 0.0f, 0.0f, 1u});
        std::array<lighting::WorldRadianceCoefficients, 2u> coefficients{unit_rows.front(), {}};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            coefficients[1].m_coefficients[channel] = 0.1f;
            coefficients[1].m_coefficients[3u + channel] = 4.0f;
        }
        auto coefficient_buffer = fixture.buffer(sizeof(coefficients), sizeof(coefficients[0]), coefficients.data());
        auto input = fixture.buffer(queries.size() * sizeof(NormalQuery), sizeof(NormalQuery), queries.data());
        auto output = fixture.buffer(queries.size() * sizeof(NormalResult), sizeof(NormalResult));
        auto counts = fixture.buffer(8u * sizeof(u32), sizeof(u32));
        rhi::ComPtr<rhi::IShaderObject> root;
        PPR_TEST_ASSERT(not make_error_code(
            fixture.m_rhi->getDevice().createRootShaderObject(fixture.m_programs[1], root.writeRef())));
        rhi::ShaderCursor cursor{root};
        const Dispatch dispatch{0u, 0u, static_cast<u32>(queries.size()), 0u};
        fixture.constant(cursor, "g_dispatch", &dispatch, sizeof(dispatch));
        fixture.bind(cursor, "g_normal_coefficients", coefficient_buffer);
        fixture.bind(cursor, "g_normal_queries", input);
        fixture.bind(cursor, "g_normal_results", output);
        fixture.bind(cursor, "g_counts", counts);

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        encoder->setBufferState(coefficient_buffer, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(input, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(counts, rhi::ResourceState::CopyDestination);
        encoder->clearBuffer(counts, 0u, 8u * sizeof(u32));
        encoder->setBufferState(counts, rhi::ResourceState::UnorderedAccess);
        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        pass->bindPipeline(fixture.m_pipelines[1], root);
        pass->dispatchCompute((static_cast<u32>(queries.size()) + 63u) / 64u, 1u, 1u);
        pass->end();
        encoder->globalBarrier();

        fixture.submit(*encoder, false);

        const auto rows = fixture.read<NormalResult>(output);
        PPR_TEST_ASSERT(rows[0].status == 0u and std::bit_cast<u32>(rows[0].value[0]) == std::bit_cast<u32>(1.0f));

        double raw_minimum = std::numeric_limits<double>::infinity();
        for (u32 index = 0u; index < constant_count; ++index) {
            const auto &query = queries[index];
            const double norm = std::sqrt(
                double(query.x) * double(query.x) +
                double(query.y) * double(query.y) +
                double(query.z) * double(query.z));
            const double expected = (1.0 + double(query.z) / norm) * 0.5;
            PPR_TEST_ASSERT(rows[index].status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::abs(double(rows[index].value[channel]) - expected) <= 0.01);
                PPR_TEST_ASSERT(rows[index].display[channel] == rows[index].value[channel]);
                raw_minimum = std::min(raw_minimum, double(rows[index].value[channel]));
            }
        }

        const auto &ringing = rows.back();
        PPR_TEST_ASSERT(ringing.status == 0u);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            PPR_TEST_ASSERT(ringing.value[channel] < -1.0f and ringing.display[channel] == 0.0f);
            raw_minimum = std::min(raw_minimum, double(ringing.value[channel]));
        }

        std::println(
            "WORLD_RC_NORMAL_RAW raw_minimum={} negative_response_magnitude={} synthetic_signed_CH_raw={} final_display={} no_AC_or_DC_clamp=1",
            raw_minimum, std::max(0.0, -raw_minimum), ringing.value[0], ringing.display[0]);
    }

    // ------------------------------------------------------------------
    // native material analytic controls on the existing qualification pipeline
    // ------------------------------------------------------------------

    void materialChecks_(Fixture &fixture) {
        std::array<lighting::WorldRadianceCoefficients, 8u> coefficients{};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            coefficients[0].m_coefficients[channel] = 1.0f;
            coefficients[1].m_coefficients[channel] = 2.0f;
            coefficients[1].m_coefficients[3u + channel] = -0.5f;
            coefficients[1].m_coefficients[12u + channel] = 0.25f;
            coefficients[1].m_coefficients[15u + channel] = 0.125f;
            coefficients[1].m_coefficients[24u + channel] = -0.2f;
            coefficients[3].m_coefficients[channel] = 8000.0f;
            coefficients[4].m_coefficients[channel] = 1.0f;
            coefficients[5].m_coefficients[channel] = -1.0f;
            coefficients[6].m_coefficients[channel] = 1.0f;
            coefficients[7].m_coefficients[channel] = 0.1f;
            coefficients[7].m_coefficients[3u + channel] = -4.0f;
        }

        coefficients[4].m_coefficients[3u] = std::numeric_limits<float>::quiet_NaN();
        coefficients[6].m_status = 1u;

        std::vector<NormalQuery> queries;
        std::vector<std::array<double, 3u>> expected;
        std::vector<bool> invalid;
        const auto append = [&](const NormalQuery query, const std::array<double, 3u> value, const bool bad = false) {
            queries.push_back(query);
            expected.push_back(value);
            invalid.push_back(bad);
        };

        for (u32 preset = 0u; preset < 3u; ++preset) {
            append({0, 0, 1, 1u | (preset << 16u)}, {2, 2, 2}); // Signed AC must vanish at the pole.
        }

        const float nz = std::sqrt(0.75f);
        for (u32 preset = 0u; preset < 3u; ++preset) {
            for (const float sign: {-1.0f, 1.0f}) {
                const float nx = 0.3f, ny = sign * 0.4f;
                const double square = double(nx) * double(nx) + double(ny) * double(ny) + double(nz) * double(nz);
                const double rho = preset == 0u ? 1.0 : preset == 1u ? 0.75 : 0.0;
                const double x = 2.0 * double(nz) * double(nx) / square * rho;
                const double y = 2.0 * double(nz) * double(ny) / square * rho;
                const double horizon = 2.0 * double(nz) * double(nz) / square - 1.0;

                // Independent closed polynomial for this signed four-order row,
                // rather than a CPU material/BRDF or cascade implementation.
                const double polynomial = 2.0 -
                                          0.5 * x +
                                          0.25 * (2.0 * x * y) +
                                          0.125 * (x * x * x - 3.0 * x * y * y) -
                                          0.2 * (4.0 * x * y * (x * x - y * y));
                const double value = std::max(polynomial, 0.0) * horizon;
                append({nx, ny, nz, 1u | (preset << 16u)}, {value, value, value});
            }
        }

        append({nz, 0, 0.5f, 0u}, {0, 0, 0}); // Reflection is below the front hemisphere.

        append({0.5f, 0, nz, 7u}, {0, 0, 0}); // Final convolution clamp, not AC clamping.

        append({0, 0, 1, 3u << 16u}, {0.52, 0.28, 0.16});
        append({0, 0, 1, 4u << 16u}, {0.5, 0.25, 0.125});
        append({0, 0, 1, 5u << 16u}, {0.5, 0.25, 0.125});

        append({1, 0, 0, 3u << 16u}, {0, 0, 0}); // Schlick reaches1; reflected hemisphere is below horizon.

        const double key = 0.04 / std::sqrt(1.41);
        append({0, 0, 1, 2u | (6u << 16u)}, {2.0 + 0.5 * key, 1.0 + 0.25 * key, 0.5 + 0.125 * key});
        append(
            {0, 0, 1, 3u | (12u << 16u)},
            {8000.0 * 0.52 + 2.0 + 0.5 * key, 8000.0 * 0.28 + 1.0 + 0.25 * key, 8000.0 * 0.16 + 0.5 + 0.125 * key});

        for (u32 preset = 7u; preset <= 11u; ++preset) {
            append({0, 0, 1, preset << 16u}, {}, true);
        }

        for (u32 index = 4u; index <= 6u; ++index) {
            append({0, 0, 1, index | (4u << 16u)}, {}, true); // Metal skip still validates all CH/status.
        }

        append({0, 0, 0, 4u << 16u}, {}, true);
        append({0, 0, -1, 4u << 16u}, {}, true);
        append({std::numeric_limits<float>::quiet_NaN(), 0, 1, 4u << 16u}, {}, true);

        auto coefficient_buffer = fixture.buffer(sizeof(coefficients), sizeof(coefficients[0]), coefficients.data());
        auto input = fixture.buffer(queries.size() * sizeof(NormalQuery), sizeof(NormalQuery), queries.data());
        auto output = fixture.buffer(queries.size() * sizeof(NormalResult), sizeof(NormalResult));
        auto counts = fixture.buffer(8u * sizeof(u32), sizeof(u32));

        rhi::ComPtr<rhi::IShaderObject> root;
        PPR_TEST_ASSERT(not make_error_code(
            fixture.m_rhi->getDevice().createRootShaderObject(fixture.m_programs[1], root.writeRef())));
        rhi::ShaderCursor cursor{root};
        const Dispatch dispatch{1u, 0u, static_cast<u32>(queries.size()), 0u};
        fixture.constant(cursor, "g_dispatch", &dispatch, sizeof(dispatch));
        fixture.bind(cursor, "g_normal_coefficients", coefficient_buffer);
        fixture.bind(cursor, "g_normal_queries", input);
        fixture.bind(cursor, "g_normal_results", output);
        fixture.bind(cursor, "g_counts", counts);

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        encoder->setBufferState(coefficient_buffer, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(input, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(counts, rhi::ResourceState::CopyDestination);
        encoder->clearBuffer(counts, 0u, 8u * sizeof(u32));
        encoder->setBufferState(counts, rhi::ResourceState::UnorderedAccess);

        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        pass->bindPipeline(fixture.m_pipelines[1], root);
        pass->dispatchCompute((static_cast<u32>(queries.size()) + 63u) / 64u, 1u, 1u);
        pass->end();
        encoder->globalBarrier();

        fixture.submit(*encoder, false);

        const auto rows = fixture.read<NormalResult>(output);
        u32 invalid_count = 0u;
        for (u32 index = 0u; index < queries.size(); ++index) {
            if (invalid[index]) {
                PPR_TEST_ASSERT(rows[index].status != 0u);
                ++invalid_count;
                continue;
            }

            PPR_TEST_ASSERT(rows[index].status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const double target = expected[index][channel];
                PPR_TEST_ASSERT(std::isfinite(rows[index].value[channel]));
                PPR_TEST_ASSERT(
                    std::abs(double(rows[index].value[channel]) - target) <= std::max(2e-5, std::abs(target) * 2e-6));
                PPR_TEST_ASSERT(rows[index].display[channel] == rows[index].value[channel]);
                PPR_TEST_ASSERT(rows[index].value[channel] <= 65504.0f);
            }
        }

        const auto aggregate = fixture.read<u32>(counts);
        PPR_TEST_ASSERT(aggregate[0] == 0u and aggregate[1] == 0u and aggregate[2] == 0u); // No transport queries.
        PPR_TEST_ASSERT(aggregate[3] == queries.size() and aggregate[4] == invalid_count);

        for (u32 index = 0u; index < 3u; ++index) {
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::bit_cast<u32>(rows[index].value[channel]) == std::bit_cast<u32>(2.0f));
            }
        }

        std::println(
            "WORLD_NATIVE_MATERIAL pole=PASS signed_four_order=PASS roughness=PASS horizon=PASS dielectric_metal=PASS key_source_once=PASS half_bound=PASS invalid_status=PASS gpu_cost=UNMEASURED");
    }

    // ------------------------------------------------------------------
    // actual native Grid consumer, crop boundaries and exposed-side selection
    // ------------------------------------------------------------------

    void worldGridChecks_(Fixture &fixture, const bool coarse = false) {
        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const lighting::OpticalCell emitter{{}, {}, {1.0, 1.0, 1.0}, lighting::OpticalKind::opaque};
        const std::array<lighting::WorldRadianceMaterial, 2> palette{{{vacuum, 0u}, {emitter, 23u}}};
        std::array<u16, 64> labels{};
        for (u32 y = 2u; y <= 5u; ++y) {
            for (u32 x = 2u; x <= 5u; ++x)
                labels[y * 8u + x] = 23u;
        }

        const auto source = lighting::WorldRadianceSource::capture(
            {labels, palette, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum, {1.0, 1.0, 1.0}});
        PPR_TEST_ASSERT(source.has_value());

        const auto generation = coarse ? std::shared_ptr<const lighting::WorldRadianceGeneration>{}
                                       : fixture.generate(
                                             *source, 35u, false,
                                             lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
                                             lighting::WorldRadianceTransport::straight256_reference);

        struct Input {
            float x{}, y{};
            u32 material{}, reserved{};
            float nx{}, ny{}, nz{};
            u32 operation{};
        };

        struct Field {
            float x{}, y{};
            u32 width{}, height{};
            i32 min_x{}, min_y{};
            u32 count_x{}, count_y{}, spacing{}, enabled{}, response{}, reserved{};
        };

        static_assert(sizeof(Input) == 32u and sizeof(Field) == 48u);
        static_assert(PPR_OFFSETOF(Field, spacing) == 32u);

        const Field field = coarse ? Field{0.0f, 0.0f, 8u, 8u, -1, -1, 4u, 4u, 4u, 1u, 1u, 0u}
                                   : Field{0.0f, 0.0f, 8u, 8u, 0, 0, 8u, 8u, 1u, 1u, 1u, 0u};
        std::vector<Input> inputs{
            {2.01f, 3.5f, 23u, 0u, 0, 0, 1, 2u}, {5.99f, 3.5f, 23u, 0u, 0, 0, 1, 2u},
            {3.5f, 3.5f, 23u, 0u, 0, 0, 1, 2u}, {2.01f, 3.5f, 23u, 0u, 1, 0, 0, 2u}};
        std::vector<float> expected{1, 1, 0, 0.5f};
        for (const float edge: {0.0f, 0.25f, 0.5f, 7.5f, 7.75f, 8.0f, -0.01f, 8.01f}) {
            inputs.push_back({edge, 4.0f, 0u, 0u, 0, 0, 1, 3u});
            inputs.push_back({4.0f, edge, 0u, 0u, 0, 0, 1, 3u});
            const float value = edge >= 0.0f and edge <= 8.0f ? 1.0f : 0.0f;
            expected.push_back(value);
            expected.push_back(value);
        }

        std::vector<u32> packed(GridPass::kCellWordsPerChunk, 0u);
        for (u32 y = 0u; y < 8u; ++y) {
            for (u32 x = 0u; x < 8u; ++x) {
                const u32 cell = y * 128u + x;
                packed[cell / 2u] |= u32{labels[y * 8u + x]} << (16u * (cell % 2u));
            }
        }

        // Tile a separate 4x4 stencil for each possible single empty cell.
        // Only the four diagonal extremities are unreachable by a face step
        // from any of the central four candidates.
        const auto stencil_start = inputs.size();
        for (u32 empty = 0u; empty <= 16u; ++empty) {
            const u32 left = 4u * empty;
            for (u32 cell = 0u; cell < 16u; ++cell) {
                if (cell == empty)
                    continue;

                const u32 index = (12u + cell / 4u) * 128u + left + cell % 4u;
                packed[index / 2u] |= 23u << (16u * (index % 2u));
            }

            inputs.push_back({float(left) + 1.5f, 13.5f, 23u, 0u, 0, 0, 1, 2u});
            expected.push_back(0.0f);
        }

        std::array<u32, GridPass::kGridTileCacheCapacity> slots{};
        slots.fill(0xffffffffu);
        slots[0] = 0u;
        auto ids = fixture.buffer(packed.size() * sizeof(u32), sizeof(u32), packed.data());
        auto lookup = fixture.buffer(sizeof(slots), sizeof(u32), slots.data());
        auto input = fixture.buffer(inputs.size() * sizeof(Input), sizeof(Input), inputs.data());
        auto output = fixture.buffer(inputs.size() * sizeof(float) * 4u, sizeof(float) * 4u);
        rhi::ComPtr<rhi::IShaderObject> root;
        PPR_TEST_ASSERT(not make_error_code(
            fixture.m_rhi->getDevice().createRootShaderObject(fixture.m_programs[2], root.writeRef())));
        rhi::ShaderCursor cursor{root};
        GridPass::GridFrame frame{};
        frame.m_cell_size_m = 1.0f;
        const std::array<u32, 4> parameters{static_cast<u32>(inputs.size()), 0, 0, 0};
        fixture.constant(cursor, "g_grid_frame", &frame, sizeof(frame));
        fixture.constant(cursor, "g_grid_world_radiance", &field, sizeof(field));
        fixture.constant(cursor, "g_grid_receiver_probe_params", parameters.data(), sizeof(parameters));
        fixture.bind(cursor, "g_cell_materials", ids);
        fixture.bind(cursor, "g_chunk_slots", lookup);
        fixture.bind(cursor, "g_grid_receiver_probe_inputs", input);
        fixture.bind(cursor, "g_grid_receiver_probe_outputs", output);
        std::array<lighting::WorldRadianceCoefficients, 16u> constant_rows{};
        for (auto &row: constant_rows) {
            for (u32 channel = 0u; channel < 3u; ++channel)
                row.m_coefficients[channel] = 1.0f;
        }

        rhi::ComPtr<rhi::IBuffer> coefficients = coarse ? fixture.buffer(
                                                              sizeof(constant_rows), sizeof(constant_rows[0]),
                                                              constant_rows.data())
                                                        : rhi::ComPtr<rhi::IBuffer>{generation->coefficientsBuffer()};
        fixture.bind(cursor, "g_grid_world_coefficients", coefficients);

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        for (auto *const buffer: {ids.get(), lookup.get(), input.get(), coefficients.get()})
            encoder->setBufferState(buffer, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        pass->bindPipeline(fixture.m_pipelines[2], root);
        pass->dispatchCompute((static_cast<u32>(inputs.size()) + 63u) / 64u, 1u, 1u);
        pass->end();
        encoder->globalBarrier();

        fixture.submit(*encoder, false);

        const auto rows = fixture.read<std::array<float, 4>>(output);
        PPR_TEST_ASSERT(rows.size() == expected.size());
        for (u32 index = 0u; index < rows.size(); ++index) {
            for (u32 channel = 0u; channel < 3u; ++channel)
                PPR_TEST_ASSERT(std::abs(rows[index][channel] - expected[index]) <= 0.01f);
        }

        PPR_TEST_ASSERT(rows[0][3] == 0.0f and rows[1][3] == 0.0f and rows[2][3] == 1.0f and rows[3][3] == 0.0f);
        for (u32 empty = 0u; empty <= 16u; ++empty) {
            const bool enclosed = empty == 0u or empty == 3u or empty == 12u or empty == 15u or empty == 16u;
            PPR_TEST_ASSERT(rows[stencil_start + empty][3] == (enclosed ? 1.0f : 0.0f));
        }

        std::println(
            "WORLD_GRID_REFERENCE crop_edges=all4 half_cell_strip=inside_only exposed_rims=2 deep_opaque_incident=0 tilted_K16=PASS");
    }

    // ------------------------------------------------------------------
    // bounded actual centre-mask/backwall pilot on the same device and passes
    // ------------------------------------------------------------------

    void screenContactChecks_(Fixture &fixture) {
        auto &device = fixture.m_rhi->getDevice();
        auto &grid = fixture.m_grid;
        constexpr u32 width = 64u, height = 128u;
        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const lighting::OpticalCell stone{{}, {}, {}, lighting::OpticalKind::opaque};
        const lighting::OpticalCell lamp{{}, {}, {2, 1, 0.5}, lighting::OpticalKind::opaque};
        const std::array<lighting::WorldRadianceMaterial, 3u> materials{{{vacuum, 0u}, {stone, 17u}, {lamp, 23u}}};

        mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(4u);
        PPR_TEST_ASSERT(not storage.materialize());
        const auto writable = storage.getMutableData();
        PPR_TEST_ASSERT(writable.has_value());
        std::ranges::fill(*writable, std::byte{255});

        mem::SharedBuffer frozen;
        PPR_TEST_ASSERT(not storage.moveToShared(&frozen));

        image::ImageAsset white;
        white.m_width = white.m_height = 1u;
        white.m_storage = frozen;
        white.m_subresources.push_back({frozen.subspan(0u, 4u), 4u, 4u});

        const auto tiny_image = [](const u32 edge, const u32 levels, const std::byte value) {
            u64 bytes = 0u;
            for (u32 level = 0u; level < levels; ++level) {
                const u64 side = image::mipExtentAt(edge, level);
                bytes += side * side * 4u;
            }

            mem::UniqueBuffer owned = mem::UniqueBuffer::allocate(safe_narrowing(bytes));
            PPR_TEST_ASSERT(not owned.materialize());
            const auto data = owned.getMutableData();
            PPR_TEST_ASSERT(data.has_value());
            std::ranges::fill(*data, value);

            image::ImageAsset result;
            result.m_width = result.m_height = edge;
            result.m_mip_count = levels;
            PPR_TEST_ASSERT(not owned.moveToShared(&result.m_storage));

            u64 offset = 0u;
            for (u32 level = 0u; level < levels; ++level) {
                const u64 side = image::mipExtentAt(edge, level), size = side * side * 4u;
                result.m_subresources.push_back(
                    {result.m_storage.subspan(safe_narrowing(offset), safe_narrowing(size)), side * 4u, size});
                offset += size;
            }

            return result;
        };

        // Match the measured 138-record closure without another application,
        // field solve or pipeline. Three unique tiny textures exercise actual
        // uploads; repeated immutable records exercise cache-local references.
        const auto gray = tiny_image(1u, 1u, std::byte{128});
        const auto coarse = tiny_image(4u, 1u, std::byte{255});
        GridStagedFrame inventory_stage;

        std::array<rhi::ComPtr<rhi::ITexture>, 2u> colors;
        std::array<rhi::ComPtr<rhi::ITextureView>, 2u> color_views;
        PPR_DEFER {
            // Both normal and assertion-unwind paths prove quiescence before
            // releasing these fixture-owned render targets and views.
            if (make_error_code(fixture.m_queue->waitOnHost())) {
                hal::fatalExit("screen-contact-fixture-quiescence-unknown");
            }
        };

        for (u32 index = 0u; index < colors.size(); ++index) {
            rhi::TextureDesc desc{};
            desc.type = index == 0u ? rhi::TextureType::Texture2D : rhi::TextureType::Texture2DMS;
            desc.sampleCount = index == 0u ? 1u : 4u;
            desc.size = {.width = width, .height = height, .depth = 1u};
            desc.format = rhi::Format::RGBA16Float;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = "screen-contact shared background target";
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, colors[index].writeRef())));
            PPR_TEST_ASSERT(not make_error_code(colors[index]->getDefaultView(color_views[index].writeRef())));
        }

        const auto camera_at = [](const float center_x, const float scale = 1.0f / 16.0f) {
            Camera camera{ECameraProjection::orthographic};
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            const float half_width = float(width) * scale * 0.5f;
            const float half_height = float(height) * scale * 0.5f;
            model.m_origin = float3{center_x - half_width, 11.0f - half_height, -0.5f};
            model.m_ortho_scale = scale;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, width, height}});

            const auto snapshot = camera.getSnapshot();
            const float4 centre = float4{0, 0, 0, 1} * snapshot.m_invert_view_projection;
            const float4 lower = float4{-1, -1, 0, 1} * snapshot.m_invert_view_projection;
            const float4 upper = float4{1, 1, 0, 1} * snapshot.m_invert_view_projection;
            PPR_TEST_ASSERT(centre.w != 0.0f and lower.w != 0.0f and upper.w != 0.0f);
            PPR_TEST_ASSERT(
                std::abs(centre.x / centre.w - center_x) <= 0.0001f and
                std::abs(centre.y / centre.w - 11.0f) <= 0.0001f);
            PPR_TEST_ASSERT(
                std::abs(lower.x / lower.w - (center_x - half_width)) <= 0.0001f and
                std::abs(lower.y / lower.w - (11.0f - half_height)) <= 0.0001f and
                std::abs(upper.x / upper.w - (center_x + half_width)) <= 0.0001f and
                std::abs(upper.y / upper.w - (11.0f + half_height)) <= 0.0001f);
            return snapshot;
        };

        const auto camera = camera_at(36.0f);
        const auto viewport = rhi::Viewport::fromSize(float(width), float(height));
        const auto scissor = rhi::ScissorRect::fromSize(width, height);
        const auto pixels = [&](rhi::ITexture *const texture, const u32 pixel_bytes) {
            shader::ComPtr<ISlangBlob> blob;
            rhi::SubresourceLayout layout{};
            PPR_TEST_ASSERT(not make_error_code(device.readTexture(texture, 0u, 0u, blob.writeRef(), &layout)));
            PPR_TEST_ASSERT(
                blob and blob->getBufferPointer() != nullptr and layout.rowPitch >= u64{width} * pixel_bytes);
            PPR_TEST_ASSERT(blob->getBufferSize() >= (height - 1u) * layout.rowPitch + u64{width} * pixel_bytes);

            std::vector<std::byte> result(std::size_t(width) * height * pixel_bytes);
            const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());
            for (u32 row = 0u; row < height; ++row) {
                std::memcpy(
                    result.data() + std::size_t(row) * width * pixel_bytes, source + row * layout.rowPitch,
                    std::size_t(width) * pixel_bytes);
            }

            return result;
        };

        const auto channel = [](const std::vector<std::byte> &bytes, const u32 index) {
            u16 bits{};
            std::memcpy(&bits, bytes.data() + std::size_t(index) * 8u, sizeof(bits));
            const u32 exponent = (bits >> 10u) & 31u, fraction = bits & 1023u;
            PPR_TEST_ASSERT(exponent != 31u);
            const double value = exponent == 0u ? std::ldexp(double(fraction), -24)
                                                : std::ldexp(1.0 + double(fraction) / 1024.0, int(exponent) - 15);
            return (bits & 32768u) != 0u ? -value : value;
        };

        const auto draw = [&](const CameraSnapshot &view, const bool pilot, const bool spatial, const bool foreground,
                              const bool measure_material = false, const bool viewport_contact = false) {
            PPR_TEST_ASSERT(not grid.update(
                {}, view, spatial, GridPass::ReceiverContinuation::nearest, GridPass::ReceiverResponse::mapped_diffuse,
                viewport_contact ? GridPass::BackgroundResponse::screen_contact
                : pilot          ? GridPass::BackgroundResponse::screen_contact_pilot
                                 : GridPass::BackgroundResponse::lit_backwall));

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            const PreRenderContext pre{device, *encoder, fixture.m_fence.completed()};
            PPR_TEST_ASSERT(not grid.prepareGpuVisibility(pre));
            PPR_TEST_ASSERT(not grid.prepareBackground(pre, view, viewport, scissor));

            rhi::ComPtr<rhi::IQueryPool> timing;
            if (measure_material) {
                rhi::QueryPoolDesc desc{};
                desc.type = rhi::QueryType::Timestamp;
                desc.count = 5u;
                PPR_TEST_ASSERT(not make_error_code(device.createQueryPool(desc, timing.writeRef())));
                PPR_TEST_ASSERT(not make_error_code(timing->reset(0u, 5u)));
                PPR_TEST_ASSERT(
                    grid.measureTerrainMaterial(pre, width, height, 7u, *timing, 1u) == std::errc::invalid_argument);
                PPR_TEST_ASSERT(not grid.measureTerrainMaterial(pre, width, height, 7u, *timing, 0u));
            }

            encoder->setTextureState(
                colors[0], rhi::SubresourceRange{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u},
                rhi::ResourceState::RenderTarget);
            if (spatial) {
                encoder->setTextureState(
                    colors[1], rhi::SubresourceRange{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u},
                    rhi::ResourceState::RenderTarget);
            }

            rhi::RenderPassColorAttachment attachment{};
            attachment.view = color_views[spatial ? 1u : 0u];
            attachment.resolveTarget = spatial ? color_views[0].get() : nullptr;
            attachment.clearValue[0] = attachment.clearValue[1] = attachment.clearValue[2] = attachment.clearValue[3] =
                0.0f;
            rhi::RenderPassDesc desc{};
            desc.colorAttachments = &attachment;
            desc.colorAttachmentCount = 1u;
            auto *const pass = encoder->beginRenderPass(desc);
            PPR_TEST_ASSERT(pass != nullptr);

            RenderPipelineSignature signature{};
            signature.m_color_formats[0] = rhi::Format::RGBA16Float;
            signature.m_color_format_count = 1u;
            signature.m_sample_count = spatial ? SampleCount::x4 : SampleCount::x1;
            const RenderPipelineKey key{signature};
            {
                PPR_DEFER {
                    pass->end();
                };

                const DrawContext context{
                    device, *pass, key, viewport, scissor, int2{int(width), int(height)}, fixture.m_fence.completed()};
                if (foreground) {
                    PPR_TEST_ASSERT(not grid.renderGpuIndirect(context));
                } else {
                    PPR_TEST_ASSERT(not grid.renderBackground(context));
                }
            }

            fixture.submit(*encoder, false);
            grid.notifyFrameSubmitted(fixture.m_fence.lastSubmitted());

            if (timing) {
                std::array<u64, 5u> ticks{};
                PPR_TEST_ASSERT(not make_error_code(timing->getResult(0u, 5u, ticks.data())));
                PPR_TEST_ASSERT(std::ranges::is_sorted(ticks) and ticks.front() < ticks.back());
            }

            return pixels(colors[0], 8u);
        };

        const auto reject_stale_background = [&](const bool spatial) {
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));

            encoder->setTextureState(
                colors[0], rhi::SubresourceRange{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u},
                rhi::ResourceState::RenderTarget);
            if (spatial) {
                encoder->setTextureState(
                    colors[1], rhi::SubresourceRange{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u},
                    rhi::ResourceState::RenderTarget);
            }

            rhi::RenderPassColorAttachment attachment{};
            attachment.view = color_views[spatial ? 1u : 0u];
            attachment.resolveTarget = spatial ? color_views[0].get() : nullptr;
            rhi::RenderPassDesc description{};
            description.colorAttachments = &attachment;
            description.colorAttachmentCount = 1u;
            auto *const pass = encoder->beginRenderPass(description);
            PPR_TEST_ASSERT(pass != nullptr);
            PPR_DEFER {
                pass->end();
            };

            RenderPipelineSignature signature{};
            signature.m_color_formats[0] = rhi::Format::RGBA16Float;
            signature.m_color_format_count = 1u;
            signature.m_sample_count = spatial ? SampleCount::x4 : SampleCount::x1;
            const RenderPipelineKey key{signature};
            const DrawContext context{
                device, *pass, key, viewport, scissor, int2{int(width), int(height)}, fixture.m_fence.completed()};
            PPR_TEST_ASSERT(grid.renderBackground(context) == std::errc::operation_not_permitted);
            // Known unsubmitted: the guarded consumer encoded no stale draw.
        };

        struct Incident {
            float r{}, g{}, b{};
            u32 status{};
        };
        static_assert(sizeof(Incident) == 16u and alignof(Incident) == 4u);
        static_assert(PPR_OFFSETOF(Incident, status) == 12u);

        std::array<std::vector<u32>, 2u> door_masks;
        std::shared_ptr<const lighting::WorldRadianceGeneration> first_generation;
        std::shared_ptr<const lighting::WorldRadianceGeneration> inventory_generation;

        for (u32 scene = 0u; scene < 3u; ++scene) {
            const bool constant = scene == 0u, open = scene == 2u;
            std::vector<u16> ids(64u * 36u, 0u);
            if (not constant) {
                for (u32 y = 9u; y <= 13u; ++y) {
                    if (not open or y != 11u)
                        ids[y * 64u + 36u] = 17u;
                }

                ids[11u * 64u + 34u] = 23u;
            }

            const auto source = lighting::WorldRadianceSource::capture(
                {ids, materials, {}, 1.0, 64u, 36u, lighting::OpticalBoundary::vacuum, {1, 1, 1}});
            PPR_TEST_ASSERT(source.has_value());

            const u64 revision = 200u + scene;
            const auto generation = fixture.generate(
                *source, revision, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
                lighting::WorldRadianceTransport::straight256_reference, 65504.0f);

            const auto coefficients = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
            PPR_TEST_ASSERT(coefficients.has_value());

            if (constant) {
                first_generation = generation;
            }

            GridStagedFrame stage{};
            stage.m_full_refresh = stage.m_reset_caches = stage.m_has_catalog = stage.m_has_background = true;
            stage.m_content_fingerprint = 13u;
            stage.m_boundary_material_id = 0u;
            stage.m_world_radiance_source = *source;
            stage.m_world_radiance_revision = revision;
            stage.m_terrain_images.assign(138u, white);
            stage.m_terrain_images[1] = gray;
            stage.m_terrain_images[2] = coarse;

            for (const auto &row: materials) {
                if (row.m_material_id == 0u) {
                    continue;
                }

                GridTerrainVisual visual{};
                visual.m_material_id = row.m_material_id;
                visual.m_color_rgba = 0xffffffffu;
                visual.m_texture_index = 0u;
                visual.m_roundness = 0.5f;
                visual.m_transport_model = row.m_material_id == 0u ? kGridTransportVacuumV1 : kGridTransportOpaqueV1;
                if (row.m_material_id == 23u) {
                    visual.m_emissive_rgb[0] = 1.0f;
                    visual.m_emissive_rgb[1] = 0.5f;
                    visual.m_emissive_rgb[2] = 0.25f;
                    visual.m_emissive_strength = 2.0f;
                }

                stage.m_terrain_visuals.push_back(visual);
            }

            for (u32 layer = 0u; layer < 4u; ++layer) {
                stage.m_background_layers[layer].m_texture_index = 0u;
                stage.m_background_layers[layer].m_order = layer;
            }

            stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridStagedUpload upload{};
            upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            for (u32 y = 0u; y < 36u; ++y) {
                for (u32 x = 0u; x < 64u; ++x)
                    upload.m_cell_materials[y * 128u + x] = ids[y * 64u + x];
            }

            stage.m_uploads.push_back(std::move(upload));

            const auto before_inventory = grid.observations();
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(7u, 11u, stage));

            const u32 expected_advances = scene == 0u ? 138u : 1u;
            for (u32 advance = 0u; advance < expected_advances; ++advance) {
                const auto ready = grid.advanceWorldCandidate(device, fixture.m_fence.completed());
                PPR_TEST_ASSERT(ready and *ready == (advance + 1u == expected_advances));
            }

            const auto after_inventory = grid.observations();
            PPR_TEST_ASSERT(
                after_inventory.m_candidate_advances - before_inventory.m_candidate_advances == expected_advances);

            if (scene == 0u) {
                PPR_TEST_ASSERT(after_inventory.m_texture_uploads - before_inventory.m_texture_uploads == 3u);
                PPR_TEST_ASSERT(
                    after_inventory.m_texture_uploaded_bytes - before_inventory.m_texture_uploaded_bytes == 72u);
            } else {
                PPR_TEST_ASSERT(
                    after_inventory.m_texture_inventory_reuses == before_inventory.m_texture_inventory_reuses + 1u);
                PPR_TEST_ASSERT(after_inventory.m_texture_uploads == before_inventory.m_texture_uploads);
                PPR_TEST_ASSERT(after_inventory.m_texture_uploaded_bytes == before_inventory.m_texture_uploaded_bytes);
                PPR_TEST_ASSERT(after_inventory.m_texture_entries == before_inventory.m_texture_entries);
                PPR_TEST_ASSERT(after_inventory.m_texture_slots == before_inventory.m_texture_slots);
                PPR_TEST_ASSERT(after_inventory.m_texture_decoded_bytes == before_inventory.m_texture_decoded_bytes);
            }

            inventory_stage = stage;
            inventory_generation = generation;

            // Prepare A immediately before the B swap, without another update
            // or preparation between activation and attempted consumption.
            const bool stale_spatial = scene == 2u and
                                       device.hasFeature("standard-sample-locations-4") and
                                       device.hasFeature("sample-rate-shading");
            if (scene != 0u) {
                PPR_TEST_ASSERT(not draw(camera, true, stale_spatial, false).empty());
            }

            PPR_TEST_ASSERT(not grid.activateWorldCandidate(7u, 11u, 13u));

            if (scene != 0u) {
                reject_stale_background(stale_spatial);
            }

            PPR_TEST_ASSERT(not grid.setWorldRadianceLighting(generation));

            const auto base = draw(camera, false, false, false);
            const auto corrected = draw(camera, false, false, false, false, true);
            const auto blocks = grid.screenContactRectForTest();
            PPR_TEST_ASSERT(
                blocks[0] == 0u and blocks[1] == 0u and blocks[2] == width / 2u and blocks[3] == height / 2u);
            const auto [viewport_mask, viewport_incident] = grid.screenContactForTest();
            PPR_TEST_ASSERT(viewport_mask and viewport_incident);
            const auto labels = pixels(viewport_mask, sizeof(u32));
            const auto records = fixture.read<Incident>(viewport_incident);

            u32 corrected_blocks{};
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    const u32 block = y / 2u * blocks[2] + x / 2u;
                    const auto &delta = records[2u * block];
                    const auto &metadata = records[2u * block + 1u];
                    PPR_TEST_ASSERT(metadata.g == 0.0f and std::isfinite(delta.r));

                    const u32 pixel = y * width + x;
                    u32 material{};
                    std::memcpy(&material, labels.data() + pixel * sizeof(u32), sizeof(u32));
                    const u32 bit = 1u << ((y % 2u) * 8u + x % 2u);
                    if (material != 0u) {
                        PPR_TEST_ASSERT((delta.status & bit) == 0u);
                        PPR_TEST_ASSERT(channel(corrected, pixel) == channel(base, pixel));
                    }

                    if (x % 2u == 1u and y % 2u == 1u and (delta.status & bit) != 0u) {
                        corrected_blocks += std::abs(delta.r) > 0.0001f;
                    }
                }
            }

            PPR_TEST_ASSERT(constant ? corrected == base : corrected != base);
            PPR_TEST_ASSERT(constant ? corrected_blocks == 0u : corrected_blocks > 0u);

            if (open) {
                const auto remote = camera_at(48.0f);
                const auto remote_base = draw(remote, false, false, false);
                PPR_TEST_ASSERT(draw(remote, false, false, false, false, true) == remote_base);
                PPR_TEST_ASSERT(grid.screenContactRectForTest() == blocks);
            }

            const auto background = draw(camera, true, false, false);
            const auto [mask_texture, incident_buffer] = grid.screenContactForTest();
            PPR_TEST_ASSERT(mask_texture != nullptr and incident_buffer != nullptr);
            const auto region = grid.screenContactRectForTest();
            PPR_TEST_ASSERT(region[2] > 0u and region[3] > 0u and u64{region[2]} * region[3] <= 32768u);
            const auto mask_bytes = pixels(mask_texture, sizeof(u32));
            std::vector<u32> mask(std::size_t(width) * height);
            std::memcpy(mask.data(), mask_bytes.data(), mask_bytes.size());
            const auto incident = fixture.read<Incident>(incident_buffer);

            for (u32 row = 0u; row < region[2] * region[3]; ++row) {
                PPR_TEST_ASSERT(incident[row].status == 0u);
                PPR_TEST_ASSERT(
                    std::isfinite(incident[row].r) and
                    std::isfinite(incident[row].g) and
                    std::isfinite(incident[row].b));

                const u32 x = row % region[2] + region[0], y = row / region[2] + region[1];
                PPR_TEST_ASSERT(
                    std::abs(channel(background, y * width + x) - std::max(double(incident[row].r), 0.0)) <= 0.003);

                if (constant) {
                    PPR_TEST_ASSERT(mask[y * width + x] == 0u and incident[row].r == 1.0f);
                }
            }

            // The4x path loads the same centre-density incident, rather than
            // repeating visibility at each sample. Constant albedo isolates it.
            if (device.hasFeature("standard-sample-locations-4") and device.hasFeature("sample-rate-shading")) {
                const auto spatial = draw(camera, true, true, false);
                PPR_TEST_ASSERT(spatial == background);
                if (not constant and not open) {
                    const auto visible = draw(camera, true, true, true);
                    const auto measured = draw(camera, true, true, true, true);
                    PPR_TEST_ASSERT(measured == visible);
                }
            } else {
                std::println("SCREEN_CONTACT spatial_consumer=NOT_RUN unsupported_standard4_or_sample_shading");
            }

            if (not constant) {
                door_masks[open ? 1u : 0u] = mask;
                // Whole foreground image is unchanged by this backwall-only flag.
                const auto control = draw(camera, false, false, true);
                const auto pilot = draw(camera, true, false, true);
                PPR_TEST_ASSERT(control == pilot);

                // At homogeneous covered/vacuum pixel neighborhoods, the actual
                // centre mask and foreground silhouette agree independently of
                // the backwall experiment's lighting values.
                u32 compared{};
                for (u32 y = 1u; y + 1u < height; ++y) {
                    for (u32 x = 1u; x + 1u < width; ++x) {
                        const u32 index = y * width + x, id = mask[index];
                        if (mask[index - 1u] != id or
                            mask[index + 1u] != id or
                            mask[index - width] != id or
                            mask[index + width] != id or
                            mask[index - width - 1u] != id or
                            mask[index - width + 1u] != id or
                            mask[index + width - 1u] != id or
                            mask[index + width + 1u] != id)
                            continue;

                        u16 alpha{};
                        std::memcpy(&alpha, control.data() + std::size_t(index) * 8u + 6u, sizeof(alpha));
                        PPR_TEST_ASSERT(id == 0u ? alpha == 0u : alpha == 0x3c00u);
                        ++compared;
                    }
                }

                PPR_TEST_ASSERT(compared > 1000u);
            }

            // Few dense bitmap-ray controls only, no CPU world/cascade solver.
            // Separate endpoint resampling from near-hit decisions and report
            // the256-tap fine-mask comparison without retuning the8-tap pilot.
            const auto far_red = [&](const double x, const double y, const double angle) {
                if (x < 0.0 or y < 0.0 or x > 64.0 or y > 36.0)
                    return 0.0;

                const double cx = std::clamp(x - 0.5, 0.0, 63.0), cy = std::clamp(y - 0.5, 0.0, 35.0);
                const u32 lx = u32(std::floor(cx)), ly = u32(std::floor(cy));
                const u32 hx = u32(std::ceil(cx)), hy = u32(std::ceil(cy));
                const double fx = cx - double(lx), fy = cy - double(ly);
                double result{};
                for (u32 tap = 0u; tap < 4u; ++tap) {
                    const double weight = ((tap & 1u) != 0u ? fx : 1.0 - fx) * ((tap & 2u) != 0u ? fy : 1.0 - fy);
                    if (weight == 0.0)
                        continue;

                    const auto &row =
                        (*coefficients)[((tap & 2u) != 0u ? hy : ly) * 64u + ((tap & 1u) != 0u ? hx : lx)];
                    double value = double(row.m_coefficients[0]);
                    for (u32 order = 1u; order <= 4u; ++order) {
                        value += double(row.m_coefficients[(2u * order - 1u) * 3u]) * std::cos(double(order) * angle) +
                                 double(row.m_coefficients[2u * order * 3u]) * std::sin(double(order) * angle);
                    }

                    result += value * weight;
                }

                return result;
            };

            for (const auto &query: {std::array<u32, 2u>{1u, 1u}, {39u, 24u}, {43u, 56u}, {1u, 126u}}) {
                const u32 px = query[0], py = query[1];
                const double qx = 34.0 + (double(px) + 0.5) / 16.0, qy = 15.0 - (double(py) + 0.5) / 16.0;
                if (mask[py * width + px] != 0u)
                    continue;

                const auto reference = [&](const u32 steps, u32 &hits, u32 &exits) {
                    double mean{};
                    for (u32 bin = 0u; bin < 16u; ++bin) {
                        const float angle = (float(bin) + 0.5f) * (2.0f * std::numbers::pi_v<float> / 16.0f);
                        const double dx = double(std::cos(angle)), dy = double(std::sin(angle));
                        const double sx = double(px) + 0.5, sy = double(py) + 0.5;
                        const double ex = 32.0 * dx, ey = -32.0 * dy;
                        double exit = 1.0;
                        if (ex != 0.0)
                            exit = std::min(exit, ((ex > 0.0 ? double(width) : 0.0) - sx) / ex);
                        if (ey != 0.0)
                            exit = std::min(exit, ((ey > 0.0 ? double(height) : 0.0) - sy) / ey);

                        bool hit{};
                        double radiance{};
                        for (u32 step = 1u; step <= steps; ++step) {
                            const double f = double(step) / double(steps);
                            if (f > exit)
                                break;

                            const i32 x = i32(std::floor(sx + ex * f)), y = i32(std::floor(sy + ey * f));
                            if (x < 0 or y < 0 or x >= i32(width) or y >= i32(height))
                                break;

                            const u32 id = mask[u32(y) * width + u32(x)];
                            if (id == 0u)
                                continue;

                            radiance = id == 23u ? 2.0 : 0.0;
                            hit = true;
                            ++hits;
                            break;
                        }

                        if (not hit) {
                            radiance = far_red(qx + 2.0 * dx * exit, qy + 2.0 * dy * exit, double(angle));
                            exits += exit < 1.0 ? 1u : 0u;
                        }

                        mean += radiance / 16.0;
                    }

                    return mean;
                };

                u32 hits{}, exits{}, dense_hits{}, dense_exits{};
                const double sampled = reference(8u, hits, exits), dense = reference(256u, dense_hits, dense_exits);
                PPR_TEST_ASSERT(
                    px >= region[0] and py >= region[1] and px < region[0] + region[2] and py < region[1] + region[3]);
                const u32 index = (py - region[1]) * region[2] + px - region[0];
                PPR_TEST_ASSERT(std::abs(double(incident[index].r) - sampled) <= 0.0001);

                std::println(
                    "SCREEN_CONTACT state={} x={} y={} sampled8={} dense256={} gpu={} hits8={} hits256={} screen_exit={} endpoint_resampling_not_isolated_visibility=1",
                    scene, px, py, sampled, dense, incident[index].r, hits, dense_hits, exits);
            }
        }

        const u32 door_center = 56u * width + 40u;
        PPR_TEST_ASSERT(door_masks[0][door_center] == 17u and door_masks[1][door_center] == 0u);
        PPR_TEST_ASSERT(grid.setWorldRadianceLighting(first_generation) == std::errc::invalid_argument);

        const auto observed = grid.observations();
        PPR_TEST_ASSERT(observed.m_screen_contact_masks > 0u and observed.m_screen_contact_bytes > 0u);

        const auto moved = draw(camera_at(36.5f), true, false, false);
        PPR_TEST_ASSERT(not moved.empty());
        const auto moved_region = grid.screenContactRectForTest();
        PPR_TEST_ASSERT(moved_region[0] != 0u or moved_region[2] != width);

        PPR_TEST_ASSERT(not draw(camera_at(36.0f, 1.0f / 32.0f), true, false, false).empty());
        const auto [zoom_mask, zoom_incident] = grid.screenContactForTest();
        PPR_TEST_ASSERT(zoom_mask != nullptr and zoom_incident != nullptr);
        const auto zoom_region = grid.screenContactRectForTest();
        const auto zoom_rows = fixture.read<Incident>(zoom_incident);
        for (u32 row = 0u; row < zoom_region[2] * zoom_region[3]; ++row) {
            PPR_TEST_ASSERT(zoom_rows[row].status == 0u and std::isfinite(zoom_rows[row].r));
        }

        PPR_TEST_ASSERT(not grid.update(
            {}, camera, false, GridPass::ReceiverContinuation::nearest, GridPass::ReceiverResponse::mapped_diffuse,
            GridPass::BackgroundResponse::screen_contact_pilot));

        rhi::ComPtr<rhi::ICommandEncoder> rejected_inverse;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(rejected_inverse.writeRef())));
        const PreRenderContext inverse_pre{device, *rejected_inverse, fixture.m_fence.completed()};
        PPR_TEST_ASSERT(not grid.prepareGpuVisibility(inverse_pre));

        auto inconsistent = camera;
        inconsistent.m_invert_view_projection[3][0] += 1.0f;

        const auto before_inverse = grid.observations();
        PPR_TEST_ASSERT(
            grid.prepareBackground(inverse_pre, inconsistent, viewport, scissor) == std::errc::not_supported);

        const auto after_inverse = grid.observations();
        PPR_TEST_ASSERT(
            before_inverse.m_draw_commands == after_inverse.m_draw_commands and
            before_inverse.m_dispatch_commands == after_inverse.m_dispatch_commands and
            before_inverse.m_screen_contact_bytes == after_inverse.m_screen_contact_bytes);

        rejected_inverse.setNull(); // Rejected before any pilot mask/refinement encode.

        PPR_TEST_ASSERT(not grid.update(
            {}, camera, false, GridPass::ReceiverContinuation::nearest, GridPass::ReceiverResponse::mapped_diffuse,
            GridPass::BackgroundResponse::screen_contact_pilot));

        rhi::ComPtr<rhi::ICommandEncoder> rejected_extent;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(rejected_extent.writeRef())));
        const PreRenderContext rejected_pre{device, *rejected_extent, fixture.m_fence.completed()};
        PPR_TEST_ASSERT(not grid.prepareGpuVisibility(rejected_pre));
        PPR_TEST_ASSERT(
            grid.prepareBackground(
                rejected_pre, camera, rhi::Viewport::fromSize(1921.0f, float(height)),
                rhi::ScissorRect::fromSize(1921u, height)) == std::errc::not_supported);

        rejected_extent.setNull(); // Known never submitted; no GPU owner escaped.

        // Accepted work protects its actual ring slot even when a later
        // caller has only stale completion knowledge. No queue-idle shortcut.
        PPR_TEST_ASSERT(not grid.update(
            {}, camera, false, GridPass::ReceiverContinuation::nearest, GridPass::ReceiverResponse::mapped_diffuse,
            GridPass::BackgroundResponse::screen_contact_pilot));

        rhi::ComPtr<rhi::ICommandEncoder> pending;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(pending.writeRef())));
        const PreRenderContext current{device, *pending, fixture.m_fence.completed()};
        PPR_TEST_ASSERT(not grid.prepareGpuVisibility(current));
        PPR_TEST_ASSERT(not grid.prepareBackground(current, camera, viewport, scissor));
        const auto [protected_mask, protected_rows] = grid.screenContactForTest();
        rhi::ComPtr<rhi::ITexture> retained_mask{protected_mask};
        rhi::ComPtr<rhi::IBuffer> retained_rows{protected_rows};

        fixture.submit(*pending, false, false);
        const auto accepted = fixture.m_fence.lastSubmitted();
        grid.notifyFrameSubmitted(accepted);

        PPR_TEST_ASSERT(not grid.update(
            {}, camera, false, GridPass::ReceiverContinuation::nearest, GridPass::ReceiverResponse::mapped_diffuse,
            GridPass::BackgroundResponse::screen_contact_pilot));

        rhi::ComPtr<rhi::ICommandEncoder> stale;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(stale.writeRef())));
        FenceCompletionValue requested_wait{kFenceValueNone};
        const PreRenderContext unknown_completion{
            device, *stale, kFenceValueNone, FenceWaitHook{[&](const FenceCompletionValue value) {
                requested_wait = value;
                return std::make_error_code(std::errc::operation_would_block);
            }}};
        PPR_TEST_ASSERT(grid.prepareGpuVisibility(unknown_completion) == std::errc::operation_would_block);
        PPR_TEST_ASSERT(requested_wait != kFenceValueNone and requested_wait <= accepted);

        stale.setNull();

        PPR_TEST_ASSERT(not fixture.m_fence.waitFor(accepted));
        PPR_TEST_ASSERT(not pixels(retained_mask, sizeof(u32)).empty());
        const auto retained = fixture.read<Incident>(retained_rows);
        PPR_TEST_ASSERT(retained[0].status == 0u);

        PPR_TEST_ASSERT(not draw(camera, true, false, false).empty());

        // Exercise inventory publication and retirement on this SAME Grid,
        // device, field and draw pipelines after the unchanged pilot checks.
        const auto drain_candidate = [&] {
            bool ready = false;
            for (u32 step = 0u; step < 512u and not ready; ++step) {
                ready = grid.worldCandidateAdmissionReady(fixture.m_fence.completed());
            }

            PPR_TEST_ASSERT(ready);
        };

        drain_candidate();
        const auto original_palette = draw(camera, false, false, true);
        auto tinted = inventory_stage;
        tinted.m_terrain_visuals[0].m_color_rgba = 0xff2020ffu;
        const auto before_palette = grid.observations();
        PPR_TEST_ASSERT(not grid.stageWorldCandidate(7u, 11u, tinted));
        const auto palette_ready = grid.advanceWorldCandidate(device, fixture.m_fence.completed());
        PPR_TEST_ASSERT(palette_ready and *palette_ready);

        const auto after_palette = grid.observations();
        PPR_TEST_ASSERT(after_palette.m_texture_inventory_reuses == before_palette.m_texture_inventory_reuses + 1u);
        PPR_TEST_ASSERT(after_palette.m_texture_uploads == before_palette.m_texture_uploads);
        PPR_TEST_ASSERT(after_palette.m_texture_uploaded_bytes == before_palette.m_texture_uploaded_bytes);
        PPR_TEST_ASSERT(after_palette.m_texture_entries == before_palette.m_texture_entries);
        PPR_TEST_ASSERT(after_palette.m_texture_decoded_bytes == before_palette.m_texture_decoded_bytes);

        PPR_TEST_ASSERT(not grid.activateWorldCandidate(7u, 11u, 13u));
        PPR_TEST_ASSERT(not grid.setWorldRadianceLighting(inventory_generation));

        const auto tinted_palette = draw(camera, false, false, true);
        PPR_TEST_ASSERT(tinted_palette != original_palette); // New palette, shared immutable texture slots.

        drain_candidate();

        // A shared candidate can be cancelled while the old reader's fence is
        // not yet proven complete. Neither lease may drain the shared cache.
        const auto before_cancel = grid.observations();
        PPR_TEST_ASSERT(not grid.stageWorldCandidate(7u, 11u, tinted));
        const auto cancel_ready = grid.advanceWorldCandidate(device, fixture.m_fence.completed());
        PPR_TEST_ASSERT(cancel_ready and *cancel_ready);

        rhi::ComPtr<rhi::ICommandEncoder> overlap;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(overlap.writeRef())));
        const PreRenderContext overlap_pre{device, *overlap, fixture.m_fence.completed()};
        PPR_TEST_ASSERT(not grid.prepareGpuVisibility(overlap_pre));
        PPR_TEST_ASSERT(not grid.prepareBackground(overlap_pre, camera, viewport, scissor));

        fixture.submit(*overlap, false, false);
        const auto overlap_fence = fixture.m_fence.lastSubmitted();
        grid.notifyFrameSubmitted(overlap_fence);
        grid.cancelWorldCandidate(7u);
        PPR_TEST_ASSERT(not grid.worldCandidateAdmissionReady(kFenceValueNone));
        const auto pending_cancel = grid.observations();
        PPR_TEST_ASSERT(pending_cancel.m_texture_entries == before_cancel.m_texture_entries);
        PPR_TEST_ASSERT(pending_cancel.m_texture_decoded_bytes == before_cancel.m_texture_decoded_bytes);
        PPR_TEST_ASSERT(pending_cancel.m_texture_uploads == before_cancel.m_texture_uploads);

        PPR_TEST_ASSERT(not fixture.m_fence.waitFor(overlap_fence));
        drain_candidate();
        PPR_TEST_ASSERT(draw(camera, false, false, true) == tinted_palette);

        // Each admitted mismatch starts the old private one-image path. The
        // active inventory and palette survive cancellation unchanged. Format
        // and its sRGB interpretation change together, as required by ImageAsset.
        for (u32 variation = 0u; variation < 6u; ++variation) {
            auto changed = tinted;
            switch (variation) {
                case 0u: changed.m_terrain_images[1] = white; break;

                case 1u: std::swap(changed.m_terrain_images[0], changed.m_terrain_images[1]); break;

                case 2u:
                    changed.m_terrain_images[1].m_format = image::ENativeImageFormat::rgba8_srgb;
                    changed.m_terrain_images[1].m_is_srgb = true;
                    break;

                case 3u: changed.m_terrain_images[2] = tiny_image(4u, 2u, std::byte{255}); break;

                case 4u: changed.m_terrain_images[1] = tiny_image(1u, 1u, std::byte{128}); break;

                case 5u: changed.m_terrain_images[2] = tiny_image(2u, 1u, std::byte{255}); break;
            }

            const auto before_miss = grid.observations();
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(7u, 11u, changed, false));
            const auto incomplete = grid.advanceWorldCandidate(device, fixture.m_fence.completed());
            PPR_TEST_ASSERT(incomplete and not *incomplete);

            const auto after_miss = grid.observations();
            PPR_TEST_ASSERT(
                after_miss.m_texture_inventory_comparisons == before_miss.m_texture_inventory_comparisons + 1u);
            PPR_TEST_ASSERT(after_miss.m_texture_inventory_reuses == before_miss.m_texture_inventory_reuses);
            PPR_TEST_ASSERT(after_miss.m_texture_uploads == before_miss.m_texture_uploads + 1u);
            PPR_TEST_ASSERT(after_miss.m_candidate_advances == before_miss.m_candidate_advances + 1u);

            rhi::ComPtr<rhi::ICommandEncoder> initialization;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(initialization.writeRef())));
            fixture.submit(*initialization, false);
            grid.notifyFrameSubmitted(fixture.m_fence.lastSubmitted());
            grid.cancelWorldCandidate(7u);
            drain_candidate();

            const auto after_drain = grid.observations();
            PPR_TEST_ASSERT(after_drain.m_texture_entries == before_miss.m_texture_entries);
            PPR_TEST_ASSERT(after_drain.m_texture_slots == before_miss.m_texture_slots);
            PPR_TEST_ASSERT(after_drain.m_texture_decoded_bytes == before_miss.m_texture_decoded_bytes);
            PPR_TEST_ASSERT(draw(camera, false, false, true) == tinted_palette);
        }

        auto bad_layout = tinted;
        bad_layout.m_terrain_images[0].m_subresources[0].m_row_pitch = 8u;
        const auto before_layout = grid.observations();
        PPR_TEST_ASSERT(grid.stageWorldCandidate(7u, 11u, bad_layout) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.observations().m_texture_uploads == before_layout.m_texture_uploads);
        PPR_TEST_ASSERT(draw(camera, false, false, true) == tinted_palette);

        std::println(
            "GRID_TEXTURE_INVENTORY cold_advances=138 warm_advances=1 warm_texture_uploads=0 warm_texture_bytes=0 palette_change=PASS reader_cancel=PASS item_order_format_mips_backing_dimensions_misses=PASS invalid_pitch=REJECTED");

        std::println(
            "SCREEN_CONTACT centre_mask=actual_displaced_selector foreground_bit_identity=PASS constant=PASS closed_open=PASS camera_rebuild=PASS per_row_status=PASS max_rows=32768 max_ray_mask_fetches=4194304 max_receiver_classification_reads=32768 temporal_history=NONE qualification=pilot_only");
    }

    // ------------------------------------------------------------------
    // production receiver windows: unchanged angular and physical controls
    // ------------------------------------------------------------------

    void directWindowChecks_(
        Fixture &fixture, const std::array<std::shared_ptr<const lighting::WorldRadianceGeneration>, 2u> &straight) {
        std::println(
            "WORLD_WINDOW_SETTINGS_LAYOUT size={} alignment={} quota_offset={}",
            sizeof(lighting::WorldRadianceSettings), alignof(lighting::WorldRadianceSettings),
            PPR_OFFSETOF(lighting::WorldRadianceSettings, m_receiver_rows_per_quantum));

        // Nonblack sky keeps every direction active in both schedules. A
        // heterogeneous receiver batch exercises signed coefficients and work
        // totals, including all shared counter slots, without mask culling.
        const lighting::OpticalCell scheduling_vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const lighting::OpticalCell scheduling_medium{{.2, .4, .8}, {.1, .3, .2}, {}, lighting::OpticalKind::absorbing};
        const lighting::OpticalCell scheduling_lamp{{}, {}, {2.0, .5, 1.0}, lighting::OpticalKind::opaque};
        const std::array<lighting::WorldRadianceMaterial, 3u> scheduling_palette{
            {{scheduling_vacuum, 0u}, {scheduling_medium, 1u}, {scheduling_lamp, 2u}}};
        std::vector<u16> scheduling_ids(10u * 9u, 0u);
        for (u32 y = 0u; y < 9u; ++y) {
            scheduling_ids[y * 10u + 4u] = 1u;
            scheduling_ids[y * 10u + 8u] = 2u;
        }

        const auto scheduling_source = lighting::WorldRadianceSource::capture(
            {scheduling_ids, scheduling_palette, {}, .5, 10u, 9u, lighting::OpticalBoundary::vacuum, {1.0, 2.0, 3.0}});
        PPR_TEST_ASSERT(scheduling_source.has_value());
        const auto scheduling_control = fixture.generate(
            *scheduling_source, 1190u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::straight256_reference);
        const auto control_rows = fixture.read<lighting::WorldRadianceCoefficients>(
            scheduling_control->coefficientsBuffer());

        const auto scheduling_window = fixture.generate(
            *scheduling_source, 1191u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {}, 3u, 32u);
        const auto window_rows = fixture.read<lighting::WorldRadianceCoefficients>(
            scheduling_window->coefficientsBuffer());
        PPR_TEST_ASSERT(control_rows.size() == scheduling_ids.size() and window_rows.size() == control_rows.size());
        for (u32 index = 0u; index < control_rows.size(); ++index) {
            const auto &control = control_rows[index];
            const auto &actual = window_rows[index];
            PPR_TEST_ASSERT(control.m_status == 0u and actual.m_status == control.m_status);

            for (u32 coefficient = 0u; coefficient < 27u; ++coefficient)
                PPR_TEST_ASSERT(
                    std::abs(actual.m_coefficients[coefficient] - control.m_coefficients[coefficient]) <= 1e-5f);
        }

        const auto &control_work = scheduling_control->statistics();
        const auto &window_work = scheduling_window->statistics();
        PPR_TEST_ASSERT(control_work.m_queries_attempted == u64{scheduling_ids.size()} * 256u);
        PPR_TEST_ASSERT(window_work.m_queries_attempted == control_work.m_queries_attempted);
        PPR_TEST_ASSERT(window_work.m_queries_completed == control_work.m_queries_completed);
        PPR_TEST_ASSERT(window_work.m_queries_failed == control_work.m_queries_failed);
        PPR_TEST_ASSERT(window_work.m_segments == control_work.m_segments and window_work.m_segments > 0u);
        PPR_TEST_ASSERT(window_work.m_skipped_blocks == control_work.m_skipped_blocks);
        PPR_TEST_ASSERT(window_work.m_rows_written == control_work.m_rows_written);
        PPR_TEST_ASSERT(window_work.m_invalid_rows == control_work.m_invalid_rows);
        PPR_TEST_ASSERT(window_work.m_status == control_work.m_status and window_work.m_status == 0u);
        PPR_TEST_ASSERT(window_work.m_rows_copied == 0u);

        double reference_sum{}, open_sum{}, closed_sum{}, error_sum{}, maximum_difference{};
        u64 traced{}, upper{};
        bool valid = true;
        for (u32 state = 0u; state < 2u; ++state) {
            const auto shape = shape_(false, state != 0u);
            const auto generation = fixture.generate(
                shape.m_source, 200u + state, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
                lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {}, {}, 512u);
            const auto rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
            PPR_TEST_ASSERT(rows.has_value());
            const auto control = fixture.read<lighting::WorldRadianceCoefficients>(
                straight[state]->coefficientsBuffer());
            traced += generation->statistics().m_queries_attempted;
            upper += u64{shape.m_width} * shape.m_height * 256u;
            for (const auto &row: *rows) {
                valid = valid and row.m_status == 0u;
                for (u32 channel = 0u; channel < 3u; ++channel)
                    valid = valid and
                            std::isfinite(row.m_coefficients[channel]) and
                            row.m_coefficients[channel] >= 0.0f;

                for (const float coefficient: row.m_coefficients)
                    valid = valid and std::isfinite(coefficient);
            }

            for (u32 screen_y = 3u; screen_y <= 6u; ++screen_y) {
                for (u32 x = 7u; x <= 9u; ++x) {
                    const std::array<double, 2> point{double(x) + 0.5, double(shape.m_height - screen_y - 1u) + 0.5};
                    const auto value = meanAt_(*generation, *rows, point);
                    const auto original = meanAt_(*straight[state], control, point);
                    const double reference = directMean_(shape, point);
                    for (u32 channel = 0u; channel < 3u; ++channel)
                        maximum_difference = std::max(maximum_difference, std::abs(value[channel] - original[channel]));

                    if (state != 0u) {
                        reference_sum += reference;
                        open_sum += value[0];
                        error_sum += std::abs(value[0] - reference);
                    } else {
                        closed_sum += value[0];
                    }

                    std::println(
                        "WORLD_WINDOW_POINT state={} x={} y={} direct1024={} straight256={} window256={}", state,
                        point[0], point[1], reference, original[0], value[0]);
                }
            }
        }

        std::println(
            "WORLD_WINDOW_PHYSICAL valid={} control_max_difference={} traced={} uncullled_bound={} open_L1={} closed={} open={} L1_gate=.15 blocked_gate=.01 fullworld_qualified=0",
            valid, maximum_difference, traced, upper, error_sum / reference_sum, closed_sum, open_sum);
        std::ignore = std::fflush(nullptr);
        PPR_TEST_ASSERT(valid and maximum_difference <= 1e-5);
        PPR_TEST_ASSERT(traced <= upper);
        PPR_TEST_ASSERT(reference_sum > 0.0 and error_sum <= .15 * reference_sum);
        PPR_TEST_ASSERT(open_sum > 0.0 and closed_sum <= .01 * open_sum);

        // Multiple real batches share one immutable output; nonblack sky must
        // trace every bin, including pages with no emitting terrain at all.
        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const std::array<lighting::WorldRadianceMaterial, 1> palette{{{vacuum, 0u}}};
        const std::vector<u16> ids(64u * 32u, 0u);
        const auto source = lighting::WorldRadianceSource::capture(
            {ids, palette, {}, .25, 64u, 32u, lighting::OpticalBoundary::vacuum, {1.0, 2.0, 3.0}});
        PPR_TEST_ASSERT(source.has_value());

        const auto generation = fixture.generate(
            *source, 202u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, true, {}, 8u);
        const auto rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(rows.has_value() and rows->size() == ids.size());
        PPR_TEST_ASSERT(generation->statistics().m_queries_attempted == u64{ids.size()} * 256u);
        for (const auto &row: *rows) {
            PPR_TEST_ASSERT(row.m_status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel)
                PPR_TEST_ASSERT(row.m_coefficients[channel] == float(channel + 1u));

            for (u32 index = 3u; index < 27u; ++index)
                PPR_TEST_ASSERT(row.m_coefficients[index] == 0.0f);
        }

        // A changed cohort forces cold work: the larger scheduling quota must
        // halve the batches while preserving every signed coefficient/status.
        const auto quota_generation = fixture.generate(
            *source, 1202u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, true, {}, 4u, 512u);
        const auto quota_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(quota_rows.has_value() and quota_rows->size() == rows->size());
        PPR_TEST_ASSERT(quota_generation->statistics().m_queries_attempted == u64{ids.size()} * 256u);

        for (u32 index = 0u; index < rows->size(); ++index) {
            const auto &control = (*rows)[index];
            const auto &actual = (*quota_rows)[index];
            PPR_TEST_ASSERT(actual.m_status == control.m_status);

            for (u32 coefficient = 0u; coefficient < 27u; ++coefficient)
                PPR_TEST_ASSERT(
                    std::abs(actual.m_coefficients[coefficient] - control.m_coefficients[coefficient]) <= 1e-5f);
        }

        // Invalid scheduling requests cannot replace an already usable field.
        lighting::WorldRadianceSettings invalid_quota{
            64u * 1024u * 1024u, 1024u * 1024u, 32u * 1024u * 1024u, 0u, .5f, true, true,
            lighting::WorldRadianceRefinedSpacing::native_cell_centres, lighting::WorldRadianceTransport::direct_window,
            65504.0f, {{0, 0}, {64u, 32u}}};
        for (const u32 quota: {0u, 31u, 1056u}) {
            invalid_quota.m_receiver_rows_per_quantum = quota;
            const auto rejected = fixture.m_pass.prepare(
                *source, quota_generation->cohort(), invalid_quota, fixture.m_fence.completed());
            PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);

            PPR_TEST_ASSERT(fixture.m_pass.pendingBatches() == 0u);
            const auto retained = fixture.m_pass.acquire(fixture.m_fence.completed());
            PPR_TEST_ASSERT(retained.has_value() and *retained == quota_generation);
        }

        const auto retained_quota_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(retained_quota_rows.has_value() and retained_quota_rows->size() == quota_rows->size());
        PPR_TEST_ASSERT(
            std::memcmp(retained_quota_rows->data(), quota_rows->data(), quota_generation->bufferBytes()) == 0);

        // A narrow emitter remains in the full immutable source well outside
        // the receiver page. Masked directions preserve exact1/256 weights.
        std::vector<u16> distant_ids(128u * 64u, 0u);
        distant_ids[50u * 128u + 111u] = 23u;
        const lighting::OpticalCell lamp{{}, {}, {2.5, 1.25, .5}, lighting::OpticalKind::opaque};
        const std::array<lighting::WorldRadianceMaterial, 2> distant_palette{{{vacuum, 0u}, {lamp, 23u}}};
        const auto distant = lighting::WorldRadianceSource::capture(
            {distant_ids, distant_palette, {}, 1.0, 128u, 64u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(distant.has_value());
        const auto all = fixture.generate(
            *distant, 203u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::straight256_reference, 65504.0f);
        const auto all_rows = fixture.read<lighting::WorldRadianceCoefficients>(all->coefficientsBuffer());

        // Partial world-edge pages use their actual width. The minimum quota
        // leaves a two-row remainder after six five-row strips in the6x32 page.
        std::vector<u16> edge_ids(70u * 65u, 0u);
        edge_ids[64u * 70u + 69u] = 23u;
        const auto edge_source = lighting::WorldRadianceSource::capture(
            {edge_ids, distant_palette, {}, 1.0, 70u, 65u, lighting::OpticalBoundary::vacuum, {1.0, 2.0, 3.0}});
        PPR_TEST_ASSERT(edge_source.has_value());

        const lighting::WorldRadianceReceiverWindow edge_window{{64, 32}, {6u, 33u}};
        const auto edge_control = fixture.generate(
            *edge_source, 1210u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, edge_window, 2u);
        const auto edge_rows = fixture.read<lighting::WorldRadianceCoefficients>(edge_control->coefficientsBuffer());
        PPR_TEST_ASSERT(edge_rows.size() == 6u * 33u);

        for (const auto [quota, batches]: std::array<std::pair<u32, u32>, 3u>{{{32u, 8u}, {512u, 2u}, {1024u, 2u}}}) {
            const auto edge = fixture.generate(
                *edge_source, 1210u + quota, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
                lighting::WorldRadianceTransport::direct_window, 65504.0f, false, edge_window, batches, quota);
            const auto actual_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
            PPR_TEST_ASSERT(actual_rows.has_value() and actual_rows->size() == edge_rows.size());
            PPR_TEST_ASSERT(edge->statistics().m_queries_attempted == u64{edge_rows.size()} * 256u);
            PPR_TEST_ASSERT(edge->statistics().m_rows_copied == 0u);

            for (u32 index = 0u; index < edge_rows.size(); ++index) {
                const auto &control = edge_rows[index];
                const auto &actual = (*actual_rows)[index];
                PPR_TEST_ASSERT(control.m_status == 0u and actual.m_status == control.m_status);

                for (u32 coefficient = 0u; coefficient < 27u; ++coefficient)
                    PPR_TEST_ASSERT(
                        std::abs(actual.m_coefficients[coefficient] - control.m_coefficients[coefficient]) <= 1e-5f);
            }
        }

        const auto page = fixture.generate(
            *distant, 204u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {{32, 0}, {32u, 32u}});
        const auto page_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(page_rows.has_value() and page_rows->size() == 1024u);
        PPR_TEST_ASSERT(
            page->statistics().m_queries_attempted > 0u and page->statistics().m_queries_attempted < 262144u);
        double maximum_error{}, energy{};
        for (u32 y = 0u; y < 32u; ++y) {
            for (u32 x = 0u; x < 32u; ++x) {
                const auto &actual = (*page_rows)[y * 32u + x];
                const auto &control = all_rows[y * 128u + 32u + x];
                PPR_TEST_ASSERT(actual.m_status == 0u and control.m_status == 0u);
                for (u32 coefficient = 0u; coefficient < 27u; ++coefficient)
                    maximum_error = std::max(
                        maximum_error,
                        std::abs(
                            double(actual.m_coefficients[coefficient]) - double(control.m_coefficients[coefficient])));

                energy += double(actual.m_coefficients[0]);
            }
        }

        std::println(
            "WORLD_WINDOW_DISTANT_SOURCE page_min=32,0 page_count=32,32 source=111,50 traced={} unculled=262144 coefficient_max_error={} energy={} full_source=128,64",
            page->statistics().m_queries_attempted, maximum_error, energy);
        PPR_TEST_ASSERT(maximum_error <= 1e-5 and energy > 0.0);

        Camera camera;
        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{40, 5, -.5f};
        model.m_ortho_scale = .25f;
        const Viewport viewport{PixelRect{0, 0, 64, 64}};
        camera.updateModel(TimeSpan{}, model, viewport);
        const auto visible = lighting::planWorldRadianceReceiverWindow(**distant, camera.getSnapshot());
        PPR_TEST_ASSERT(visible.has_value());
        PPR_TEST_ASSERT(visible->m_minimum == (std::array<i32, 2>{32, 0}));
        PPR_TEST_ASSERT(visible->m_counts == (std::array<u32, 2>{32u, 32u}));

        const std::array<lighting::WorldRadianceReceiverWindow, 1> offscreen{
            lighting::WorldRadianceReceiverWindow{{108, 47}, {1u, 1u}}};
        const auto requested = lighting::planWorldRadianceReceiverWindow(**distant, camera.getSnapshot(), offscreen);
        PPR_TEST_ASSERT(requested.has_value());
        PPR_TEST_ASSERT(requested->m_minimum == (std::array<i32, 2>{32, 0}));
        PPR_TEST_ASSERT(requested->m_counts == (std::array<u32, 2>{96u, 64u}));

        // A wholly exterior view requests no rows, while an explicit offscreen
        // gameplay receiver remains authoritative even with that same camera.
        model.m_origin = float3{1000, 1000, -.5f};
        camera.updateModel(TimeSpan{}, model, viewport);
        const auto exterior = lighting::planWorldRadianceReceiverWindow(**distant, camera.getSnapshot());
        PPR_TEST_ASSERT(exterior.has_value() and exterior->m_counts == (std::array<u32, 2>{0u, 0u}));
        const auto exterior_demand = lighting::planWorldRadianceReceiverWindow(
            **distant, camera.getSnapshot(), offscreen);
        PPR_TEST_ASSERT(exterior_demand.has_value());
        PPR_TEST_ASSERT(exterior_demand->m_minimum == (std::array<i32, 2>{96, 32}));
        PPR_TEST_ASSERT(exterior_demand->m_counts == (std::array<u32, 2>{32u, 32u}));

        // Reject an oversized window on this SAME source/cohort, then publish
        // a different nonresident page. A rejected admission cannot poison the
        // existing producer or require another device/pipeline family.
        const lighting::WorldRadianceSettings oversized{
            64u * 1024u * 1024u, 1024u * 1024u, 128u * 1024u, 0u, .5f, true, false,
            lighting::WorldRadianceRefinedSpacing::native_cell_centres, lighting::WorldRadianceTransport::direct_window,
            65504.0f, {{0, 0}, {128u, 64u}}};
        const auto oversized_admission = fixture.m_pass.prepare(
            *distant, page->cohort(), oversized, fixture.m_fence.completed());
        PPR_TEST_ASSERT(
            oversized_admission.has_value() and oversized_admission->m_admission_error == std::errc::no_buffer_space);

        const auto recovered = fixture.generate(
            *distant, 204u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {{96, 32}, {32u, 32u}});
        PPR_TEST_ASSERT(recovered->source() == page->source());
        PPR_TEST_ASSERT(recovered->cohort().m_geometry_revision == page->cohort().m_geometry_revision);
        const auto recovered_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(recovered_rows.has_value() and recovered_rows->size() == 1024u);
        for (const auto &row: *recovered_rows) {
            PPR_TEST_ASSERT(row.m_status == 0u);
            for (const float coefficient: row.m_coefficients)
                PPR_TEST_ASSERT(std::isfinite(coefficient));
        }

        // Exact immutable overlap is copied into a new owned window. Fresh
        // strips preserve the cold result, including every signed coefficient.
        const lighting::WorldRadianceReceiverWindow window_a{{0, 0}, {64u, 32u}};
        const lighting::WorldRadianceReceiverWindow window_b{{32, 0}, {64u, 32u}};
        const auto donor = fixture.generate(
            *distant, 205u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, window_a, 8u);
        const auto donor_rows = fixture.read<lighting::WorldRadianceCoefficients>(donor->coefficientsBuffer());
        const auto warm = fixture.generate(
            *distant, 205u, true, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, window_b, 2u, 512u);
        PPR_TEST_ASSERT(warm->statistics().m_rows_copied == 1024u);
        const auto warm_rows = fixture.read<lighting::WorldRadianceCoefficients>(warm->coefficientsBuffer());

        const auto covered = fixture.generate(
            *distant, 205u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, window_b, 1u, 1024u);
        PPR_TEST_ASSERT(covered->statistics().m_rows_copied == 2048u);
        PPR_TEST_ASSERT(covered->statistics().m_queries_attempted == 0u);
        const auto covered_rows = fixture.read<lighting::WorldRadianceCoefficients>(covered->coefficientsBuffer());
        PPR_TEST_ASSERT(std::memcmp(warm_rows.data(), covered_rows.data(), warm->bufferBytes()) == 0);

        const auto changed_ceiling = fixture.generate(
            *distant, 205u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65503.0f, false, window_b);
        PPR_TEST_ASSERT(changed_ceiling->statistics().m_rows_copied == 0u);
        PPR_TEST_ASSERT(changed_ceiling->statistics().m_queries_attempted > 0u);

        const auto cold = fixture.generate(
            *distant, 206u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65503.0f, false, window_b);
        PPR_TEST_ASSERT(cold->statistics().m_rows_copied == 0u);
        const auto cold_rows = fixture.read<lighting::WorldRadianceCoefficients>(cold->coefficientsBuffer());
        PPR_TEST_ASSERT(cold_rows.size() == warm_rows.size());
        PPR_TEST_ASSERT(std::memcmp(warm_rows.data(), cold_rows.data(), cold->bufferBytes()) == 0);
        PPR_TEST_ASSERT(warm->statistics().m_queries_attempted < cold->statistics().m_queries_attempted);

        const auto changed_source = lighting::WorldRadianceSource::capture(
            {distant_ids, distant_palette, {}, 1.0, 128u, 64u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(changed_source.has_value() and changed_source->get() != distant->get());
        const auto source_miss = fixture.generate(
            *changed_source, 206u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65503.0f, false, window_b);
        PPR_TEST_ASSERT(source_miss->statistics().m_rows_copied == 0u);
        PPR_TEST_ASSERT(source_miss->statistics().m_queries_attempted == cold->statistics().m_queries_attempted);

        const auto retained_donor = fixture.read<lighting::WorldRadianceCoefficients>(donor->coefficientsBuffer());
        PPR_TEST_ASSERT(std::memcmp(donor_rows.data(), retained_donor.data(), donor->bufferBytes()) == 0);
        std::println(
            "WORLD_WINDOW_REUSE overlap_rows={} fresh_rows={} copied_only_rows={} warm_queries={} cold_queries={} exact_cold_bytes=checked cohort_miss=checked response_miss=checked source_miss=checked donor_unchanged=checked",
            warm->statistics().m_rows_copied, warm->statistics().m_rows_written - warm->statistics().m_rows_copied,
            covered->statistics().m_rows_copied, warm->statistics().m_queries_attempted,
            cold->statistics().m_queries_attempted);

        // Complete4096 source support is distinct from all-pages field storage.
        // The unmodified512MiB resource ceiling rejects112B*4096² before any
        // allocation/encode; this is explicitly NOT full-world budget success.
        std::vector<u16> world_ids(4096u * 4096u, 0u);
        world_ids[4000u * 4096u + 4000u] = 23u;
        const auto world = lighting::WorldRadianceSource::capture(
            {world_ids, distant_palette, {}, 1.0, 4096u, 4096u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(world.has_value());
        const lighting::WorldRadianceSettings complete{
            4ull * 1024u * 1024u * 1024u, 1024ull * 1024u * 1024u, 512ull * 1024u * 1024u, 0u, .5f, false, false,
            lighting::WorldRadianceRefinedSpacing::native_cell_centres, lighting::WorldRadianceTransport::direct_window,
            65504.0f, {{0, 0}, {4096u, 4096u}}};
        const auto admission = fixture.m_pass.prepare(
            *world, {7u, 11u, 13u, 205u}, complete, fixture.m_fence.completed());
        PPR_TEST_ASSERT(admission.has_value() and admission->m_admission_error == std::errc::no_buffer_space);
        PPR_TEST_ASSERT(admission->m_expected_rows == 16777216ull);
        PPR_TEST_ASSERT(admission->m_refined_bytes == 1879048192ull);
        std::println(
            "WORLD_ALL_PAGES_STRESS receivers={} field_bytes={} peak_bytes={} admission=REJECTED GPU_work=UNRUN fullworld_budget_qualified=0",
            admission->m_expected_rows, admission->m_refined_bytes, admission->m_peak_bytes);
    }

    // ------------------------------------------------------------------
    // one reflected bounce: direct identity, independent room bounds and offscreen keys
    // ------------------------------------------------------------------

    std::array<double, 3u> nativeCornerBounce_(
        Fixture &fixture, const std::shared_ptr<const lighting::WorldRadianceSource> &source,
        const std::span<const lighting::WorldRadianceMaterial> source_materials, const u64 revision) {
        PPR_TEST_ASSERT(source->width() == 32u and source->height() == 32u and source->cellMetres() == 1.0);
        const auto labels = source->materialIds();
        std::vector<u32> packed((labels.size() + 1u) / 2u);
        for (u32 index = 0u; index < labels.size(); ++index)
            packed[index / 2u] |= u32{labels[index]} << (16u * (index % 2u));

        std::vector<Material> palette(source_materials.size());
        for (const auto &row: source_materials) {
            PPR_TEST_ASSERT(row.m_material_id < palette.size());

            auto &material = palette[row.m_material_id];
            material.kind = row.m_optics.m_kind == lighting::OpticalKind::vacuum   ? 0u
                            : row.m_optics.m_kind == lighting::OpticalKind::opaque ? 1u
                                                                                   : 2u;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                material.source[channel] = static_cast<float>(row.m_optics.m_surface_emission[channel]);
                material.sigma[channel] = static_cast<float>(row.m_optics.m_extinction[channel]);
                material.emission[channel] = static_cast<float>(row.m_optics.m_volume_emission[channel]);
                material.m_albedo[channel] = row.m_diffuse_albedo[channel];
            }
        }

        std::array<Query, 256u> queries{};
        std::array<std::array<float, 2u>, 256u> directions{};
        for (u32 bin = 0u; bin < queries.size(); ++bin) {
            const double angle = 2.0 * std::numbers::pi * (double(bin) + .5) / 256.0;
            directions[bin] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
            queries[bin] = {8.5f, 12.5f, directions[bin][0], directions[bin][1], 0.0f, 128.0f, 1u, 1u};
        }

        // One native level uses the SAME DDA without hierarchy acceleration.
        // The first256 actual rays provide the complete required face-key union;
        // no emitter mask, source-based pruning or CPU transport is involved.
        const std::vector<u32> vacuum_flags(labels.size(), 0u);
        const Hierarchy hierarchy{0u, 32u, 32u, 1u};
        const Parameters parameters{32u, 32u, static_cast<u32>(palette.size()), 1u, {}, 68u};
        auto ids = fixture.buffer(packed.size() * sizeof(u32), sizeof(u32), packed.data());
        auto materials = fixture.buffer(palette.size() * sizeof(Material), sizeof(Material), palette.data());
        auto vacuum = fixture.buffer(vacuum_flags.size() * sizeof(u32), sizeof(u32), vacuum_flags.data());
        auto metadata = fixture.buffer(sizeof(hierarchy), sizeof(hierarchy), &hierarchy);
        auto rays = fixture.buffer(sizeof(queries), sizeof(Query), queries.data());
        auto direction_buffer = fixture.buffer(sizeof(directions), sizeof(directions[0]), directions.data());
        auto output = fixture.buffer(queries.size() * sizeof(Interval), sizeof(Interval));
        auto counts = fixture.buffer(8u * sizeof(u32), sizeof(u32));
        auto keys_buffer = fixture.m_unused_face_keys;
        auto lookup = fixture.m_unused_face_lookup;
        auto moments = fixture.m_unused_face_moments;
        BounceDispatch bounce{};

        const auto dispatch = [&](const u32 program, const u32 operation, const u32 rows, const u32 expected_queries,
                                  const u32 first = 0u) {
            bounce.first = first;
            const Dispatch work{operation, 0u, rows, 0u};
            rhi::ComPtr<rhi::IShaderObject> root;
            PPR_TEST_ASSERT(not make_error_code(
                fixture.m_rhi->getDevice().createRootShaderObject(fixture.m_programs[program], root.writeRef())));
            rhi::ShaderCursor cursor{root};
            fixture.constant(cursor, "g_world", &parameters, sizeof(parameters));
            fixture.constant(cursor, "g_dispatch", &work, sizeof(work));
            fixture.constant(cursor, "g_bounce", &bounce, sizeof(bounce));
            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 11u>{
                     std::pair{"g_cell_ids", ids.get()}, {"g_materials", materials.get()}, {"g_vacuum", vacuum.get()},
                     {"g_hierarchy", metadata.get()}, {"g_queries", rays.get()}, {"g_current", output.get()},
                     {"g_counts", counts.get()}, {"g_directions", direction_buffer.get()},
                     {"g_face_keys", keys_buffer.get()}, {"g_face_lookup", lookup.get()},
                     {"g_face_moments", moments.get()}})
                fixture.bind(cursor, name, buffer);

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            for (auto *const buffer:
                {ids.get(), materials.get(), vacuum.get(), metadata.get(), rays.get(), direction_buffer.get(),
                    keys_buffer.get()})
                encoder->setBufferState(buffer, rhi::ResourceState::ShaderResource);

            encoder->setBufferState(counts, rhi::ResourceState::CopyDestination);
            encoder->clearBuffer(counts, 0u, 8u * sizeof(u32));
            if (program == 3u) {
                encoder->setBufferState(lookup, rhi::ResourceState::CopyDestination);
                encoder->clearBuffer(lookup, 0u, lookup->getDesc().size);
            }

            encoder->setBufferState(counts, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(lookup, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(moments, rhi::ResourceState::UnorderedAccess);
            encoder->globalBarrier();

            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(fixture.m_pipelines[program], root);
            pass->dispatchCompute(program == 5u ? rows : (rows + 63u) / 64u, 1u, 1u);
            pass->end();
            encoder->globalBarrier();
            fixture.submit(*encoder, false);
            PPR_TEST_ASSERT(slotFenceComplete(fixture.m_fence.completed(), fixture.m_fence.lastSubmitted()));

            const auto actual = fixture.read<u32>(counts);
            PPR_TEST_ASSERT(actual.size() == 8u);
            PPR_TEST_ASSERT(actual[0] == expected_queries and actual[1] == expected_queries and actual[2] == 0u);
            PPR_TEST_ASSERT(actual[3] == rows and actual[4] == 0u and actual[7] == 0u);
            return actual;
        };

        dispatch(0u, 0u, 256u, 256u);
        const auto discovered = fixture.read<Interval>(output);
        std::vector<u32> keys;
        for (const auto &row: discovered) {
            PPR_TEST_ASSERT(row.status == 0u);
            if (row.segments != 0xffffffffu) {
                PPR_TEST_ASSERT((row.segments >> 2u) < labels.size());
                keys.push_back(row.segments);
            }
        }

        std::ranges::sort(keys);
        keys.erase(std::ranges::unique(keys).begin(), keys.end());

        PPR_TEST_ASSERT(not keys.empty() and keys.size() <= 256u);

        bounce.faces = static_cast<u32>(keys.size());
        bounce.capacity = 1u;
        while (bounce.capacity < 2u * bounce.faces)
            bounce.capacity *= 2u;

        keys_buffer = fixture.buffer(keys.size() * sizeof(u32), sizeof(u32), keys.data());
        lookup = fixture.buffer(bounce.capacity * sizeof(u32), sizeof(u32));
        moments = fixture.buffer(keys.size() * sizeof(std::array<float, 3u>), sizeof(std::array<float, 3u>));
        dispatch(3u, 0u, bounce.faces, 0u);
        const auto serial_counts = dispatch(4u, 0u, bounce.faces, 128u * bounce.faces);
        const auto serial_moments = fixture.read<std::array<float, 3u>>(moments);
        dispatch(0u, 1u, 256u, 256u);
        const auto serial_reflected = fixture.read<Interval>(output);

        PPR_TEST_ASSERT(bounce.faces > 1u);
        auto parallel_counts = dispatch(5u, 0u, 1u, 128u);
        const auto tail_counts = dispatch(5u, 0u, bounce.faces - 1u, 128u * (bounce.faces - 1u), 1u);
        for (u32 counter = 0u; counter < 8u; ++counter)
            parallel_counts[counter] += tail_counts[counter];

        PPR_TEST_ASSERT(parallel_counts == serial_counts);
        const auto parallel_moments = fixture.read<std::array<float, 3u>>(moments);
        for (u32 face = 0u; face < bounce.faces; ++face) {
            for (u32 channel = 0u; channel < 3u; ++channel)
                PPR_TEST_ASSERT(std::abs(parallel_moments[face][channel] - serial_moments[face][channel]) <= 1e-5f);
        }

        dispatch(0u, 1u, 256u, 256u);
        const auto reflected = fixture.read<Interval>(output);
        for (u32 ray = 0u; ray < reflected.size(); ++ray) {
            PPR_TEST_ASSERT(reflected[ray].status == serial_reflected[ray].status);
            for (u32 channel = 0u; channel < 3u; ++channel)
                PPR_TEST_ASSERT(
                    std::abs(reflected[ray].source[channel] - serial_reflected[ray].source[channel]) <= 1e-5f);
        }

        std::array<double, 3u> result{};
        for (const auto &row: reflected) {
            PPR_TEST_ASSERT(row.status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::isfinite(row.source[channel]) and row.source[channel] >= 0.0f);
                PPR_TEST_ASSERT(std::isfinite(row.transmission[channel]) and row.transmission[channel] == 0.0f);
                result[channel] += double(row.source[channel]) / 256.0;
            }
        }

        std::println(
            "WORLD_SINGLE_BOUNCE_NATIVE_CONTROL revision={} source_instance={} receiver=(8.5,12.5) RGB=({},{},{}) faces={} discovery_rays=256 evaluation_rays=256 moment_rays={} total_rays={} lookup_capacity={} control_only=true",
            revision, reinterpret_cast<std::uintptr_t>(source.get()), result[0], result[1], result[2], bounce.faces,
            128u * bounce.faces, 512u + 128u * bounce.faces, bounce.capacity);
        return result;
    }

    // ------------------------------------------------------------------
    // serial and parallel face moments across native optical inputs
    // ------------------------------------------------------------------

    void faceMomentParityChecks_(Fixture &fixture) {
        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const lighting::OpticalCell wall{{}, {}, {4, 2, 1}, lighting::OpticalKind::opaque};
        const lighting::OpticalCell medium{{.1, .3, .7}, {.2, .1, .4}, {}, lighting::OpticalKind::absorbing};
        const std::array<lighting::WorldRadianceOverlayCell, 1u> partial{
            lighting::WorldRadianceOverlayCell{17u, .25f, {1, 3, 2}, {.5f, .4f, .3f}}};
        const std::array<Query, 1u> rays{Query{.5f, 1.5f, 1, 0, 0, 14, 0u, 2u}};

        for (const bool nonblack_sky: {false, true}) {
            for (const bool absorbing: {false, true}) {
                Shape shape;
                shape.m_width = 16u;
                shape.m_height = 16u;
                shape.m_ids.assign(256u, absorbing ? 2u : 0u);
                shape.m_materials = {{vacuum, 0u}, {wall, 1u}, {medium, 2u}};

                std::vector<u32> keys;
                for (u32 y = 2u; y <= 11u; y += 3u) {
                    for (u32 x = 2u; x <= 11u; x += 3u) {
                        const u32 cell = y * 16u + x;
                        shape.m_ids[cell] = 1u;
                        for (u32 side = 0u; side < 4u; ++side)
                            keys.push_back(cell * 4u + side);
                    }
                }

                for (u32 side = 0u; side < 4u; ++side)
                    keys.push_back(17u * 4u + side);

                // 68 distinct faces cross the old64-face group boundary and
                // explicitly include every normal for hard and partial owners.
                PPR_TEST_ASSERT(keys.size() == 68u);
                const std::vector<std::array<float, 3u>> zero_moments(keys.size());
                OverlayTrace serial{partial, keys, zero_moments, {}, {}};
                serial.m_bake_moments = true;
                serial.m_sky = nonblack_sky ? std::array{.3f, .7f, 1.1f} : std::array<float, 3u>{};
                const auto [serial_rows, serial_counts] = traceRows_(fixture, shape, rays, false, true, &serial);

                OverlayTrace parallel{partial, keys, zero_moments, {}, {}};
                parallel.m_bake_moments = true;
                parallel.m_parallel_moments = true;
                parallel.m_sky = serial.m_sky;
                const auto [parallel_rows, parallel_counts] = traceRows_(fixture, shape, rays, false, true, &parallel);
                PPR_TEST_ASSERT(serial_counts == parallel_counts and serial_counts[7] == 0u);
                PPR_TEST_ASSERT(serial_counts[0] == 68u * 128u + 1u);
                PPR_TEST_ASSERT(serial.m_baked_moments.size() == keys.size());
                PPR_TEST_ASSERT(parallel.m_baked_moments.size() == keys.size());

                for (u32 face = 0u; face < keys.size(); ++face) {
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        const float expected = serial.m_baked_moments[face][channel];
                        const float actual = parallel.m_baked_moments[face][channel];
                        PPR_TEST_ASSERT(std::isfinite(actual) and std::abs(actual - expected) <= 1e-5f);
                    }
                }

                PPR_TEST_ASSERT(serial_rows[0].status == parallel_rows[0].status);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT(
                        std::abs(serial_rows[0].source[channel] - parallel_rows[0].source[channel]) <= 1e-5f);
                    PPR_TEST_ASSERT(serial_rows[0].transmission[channel] == parallel_rows[0].transmission[channel]);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // one reflected bounce controls
    // ------------------------------------------------------------------

    void singleBounceChecks_(Fixture &fixture) {
        faceMomentParityChecks_(fixture);

        GridTerrainVisual visual{};
        visual.m_color_rgba = 0xffffffffu;
        const auto white_albedo = gridMaterialDiffuseAlbedo(visual, {});
        PPR_TEST_ASSERT(white_albedo.has_value() and *white_albedo == (std::array{1.0f, 1.0f, 1.0f}));
        visual.m_metallic = 1.0f;
        const auto metal_albedo = gridMaterialDiffuseAlbedo(visual, {});
        PPR_TEST_ASSERT(metal_albedo.has_value() and *metal_albedo == (std::array{0.0f, 0.0f, 0.0f}));
        visual.m_texture_index = 0u;
        const auto unavailable_albedo = gridMaterialDiffuseAlbedo(visual, {});
        PPR_TEST_ASSERT(not unavailable_albedo and unavailable_albedo.error() == std::errc::invalid_argument);

        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const lighting::OpticalCell white{{}, {}, {1, 1, 1}, lighting::OpticalKind::opaque};

        // A reflected continuation terminates at its FIRST world exit or a
        // strict exterior origin. The exact lower boundary accepts inward
        // native ownership and rejects outward ownership; primary is unchanged.
        Shape boundary;
        boundary.m_width = 4u;
        boundary.m_height = 2u;
        boundary.m_ids.assign(8u, 0u);
        boundary.m_materials.push_back({vacuum, 0u});
        const std::array<Query, 4u> boundary_queries{
            Query{-1.0f, .5f, 1, 0, 0, 2}, Query{.5f, .5f, -1, 0, 0, 1}, Query{0, .5f, 1, 0, 0, .25f},
            Query{0, .5f, -1, 0, 0, .25f}};
        const auto [boundary_primary, primary_work] = traceRows_(fixture, boundary, boundary_queries);
        const auto [boundary_secondary, secondary_work] = traceRows_(fixture, boundary, boundary_queries, false, true);
        PPR_TEST_ASSERT(
            primary_work[0] == 4u and primary_work[1] == 4u and primary_work[2] == 0u and primary_work[7] == 0u);
        PPR_TEST_ASSERT(
            secondary_work[0] == 4u and
            secondary_work[1] == 4u and
            secondary_work[2] == 0u and
            secondary_work[7] == 0u);

        for (u32 ray = 0u; ray < boundary_queries.size(); ++ray) {
            PPR_TEST_ASSERT(boundary_primary[ray].status == 0u and boundary_secondary[ray].status == 0u);

            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(boundary_primary[ray].source[channel] == 0.0f);
                PPR_TEST_ASSERT(boundary_primary[ray].transmission[channel] == 1.0f);
                PPR_TEST_ASSERT(boundary_secondary[ray].source[channel] == 0.0f);
                PPR_TEST_ASSERT(boundary_secondary[ray].transmission[channel] == (ray == 2u ? 1.0f : 0.0f));
            }
        }

        std::println(
            "WORLD_SINGLE_BOUNCE_BOUNDARY strict_exterior=black first_exit=terminated inward_boundary=accepted outward_boundary=terminated primary_unchanged=checked");

        const auto room_source = [&](const std::array<float, 3u> albedo, const bool absorbing) {
            std::vector<u16> ids(8u * 8u);
            for (u32 y = 2u; y <= 5u; ++y) {
                for (u32 x = 2u; x <= 5u; ++x)
                    ids[y * 8u + x] = x == 2u or x == 5u or y == 2u or y == 5u ? 1u : absorbing ? 2u : 0u;
            }

            // j=sigma*1 and opaque S=1 make PRIMARY incident exactly1 inside
            // the sealed room. SECONDARY keeps sigma but must NOT reinject j.
            const lighting::OpticalCell medium{{0, .5, 1.5}, {0, .5, 1.5}, {}, lighting::OpticalKind::absorbing};
            const std::array<lighting::WorldRadianceMaterial, 3u> palette{
                {{vacuum, 0u}, {white, 1u, albedo}, {medium, 2u}}};
            const auto source = lighting::WorldRadianceSource::capture(
                {ids, palette, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum, {.1, .2, .3}});
            PPR_TEST_ASSERT(source.has_value());
            return *source;
        };

        const auto black = room_source({}, true);
        lighting::WorldRadianceSupportCounts sky_support{};
        const auto [zero_direct, zero_combined, zero_discovery] = fixture.generateBounce(
            black, 701u, {{0, 0}, {8u, 8u}}, 701u, false, &sky_support);
        const auto direct_zero_rows = fixture.read<lighting::WorldRadianceCoefficients>(
            zero_direct->coefficientsBuffer());
        const auto combined_zero_rows = fixture.read<lighting::WorldRadianceCoefficients>(
            zero_combined->coefficientsBuffer());
        PPR_TEST_ASSERT(
            std::memcmp(direct_zero_rows.data(), combined_zero_rows.data(), zero_direct->bufferBytes()) == 0);
        PPR_TEST_ASSERT(zero_discovery->faceCount() > 0u);

        // A nonzero sky conservatively retains EVERY already masked bin and
        // all128 outward face bins, independent of source bounds/occlusion.
        PPR_TEST_ASSERT(sky_support.m_direct_baseline == 64u * 256u);
        PPR_TEST_ASSERT(sky_support.m_direct_included == sky_support.m_direct_baseline);
        PPR_TEST_ASSERT(sky_support.m_moment_baseline == u64{zero_discovery->faceCount()} * 128u);
        PPR_TEST_ASSERT(sky_support.m_moment_included == sky_support.m_moment_baseline);
        PPR_TEST_ASSERT(sky_support.m_support_tests == 0u and not sky_support.m_cpu_measured);
        PPR_TEST_ASSERT(sky_support.m_cpu_key_capacity_bytes >= u64{zero_discovery->faceCount()} * sizeof(u32));

        const std::array<float, 3u> albedo{.25f, .5f, .75f};
        const auto colour = room_source(albedo, false);
        const auto [colour_direct, colour_combined, colour_discovery] = fixture.generateBounce(
            colour, 702u, {{0, 0}, {8u, 8u}}, 702u);
        const auto colour_rows = fixture.read<lighting::WorldRadianceCoefficients>(
            colour_combined->coefficientsBuffer());
        // Closed-form finite midpoint K quadrature for a cardinal face. The
        // existing normalized kernel is (pi/2)*max(cos,0), averaged over256;
        // sum of the128 positive midpoint cosines is1/sin(pi/256). No extra/pi.
        const double gain = std::numbers::pi / (512.0 * std::sin(std::numbers::pi / 256.0));
        for (u32 y = 3u; y <= 4u; ++y) {
            for (u32 x = 3u; x <= 4u; ++x) {
                const auto &row = colour_rows[y * 8u + x];
                PPR_TEST_ASSERT(row.m_status == 0u);
                for (u32 channel = 0u; channel < 3u; ++channel)
                    PPR_TEST_ASSERT(
                        std::abs(double(row.m_coefficients[channel]) - (1.0 + double(albedo[channel]) * gain)) <= 2e-5);
            }
        }

        const auto absorbing = room_source(albedo, true);
        const auto [medium_direct, medium_combined, medium_discovery] = fixture.generateBounce(
            absorbing, 703u, {{0, 0}, {8u, 8u}}, 703u);
        const auto medium_rows = fixture.read<lighting::WorldRadianceCoefficients>(
            medium_combined->coefficientsBuffer());
        const auto primary_rows = fixture.read<lighting::WorldRadianceCoefficients>(
            medium_direct->coefficientsBuffer());
        constexpr std::array sigma{0.0, .5, 1.5};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // At(3.5,3.5) every segment to the enclosing[3,5]^2 wall is
            // between.5 and sqrt(4.5) metres; parent T is zero at that first hit.
            const double reflected = double(medium_rows[3u * 8u + 3u].m_coefficients[channel]) -
                                     double(primary_rows[3u * 8u + 3u].m_coefficients[channel]);
            const double incident = double(albedo[channel]) * gain;
            PPR_TEST_ASSERT(medium_rows[3u * 8u + 3u].m_status == 0u);
            PPR_TEST_ASSERT(reflected >= incident * std::exp(-sigma[channel] * std::sqrt(4.5)) - 2e-5);
            PPR_TEST_ASSERT(reflected <= incident * std::exp(-sigma[channel] * .5) + 2e-5);
        }

        // The opaque partition seals the left component. Opening above y21
        // exposes a reflective ceiling to the right-hand lamp around the corner;
        // the receiver-to-lamp line remains blocked in BOTH states.
        std::array<double, 2u> corner{};
        std::array<double, 2u> native_corner{};
        for (u32 open = 0u; open < 2u; ++open) {
            std::vector<u16> ids(32u * 32u);
            for (u32 y = 0u; y < (open != 0u ? 21u : 32u); ++y)
                ids[y * 32u + 16u] = 1u;
            for (u32 x = 8u; x <= 24u; ++x)
                ids[26u * 32u + x] = 1u;
            ids[12u * 32u + 24u] = 2u;

            const lighting::OpticalCell wall{{}, {}, {}, lighting::OpticalKind::opaque};
            const std::array<lighting::WorldRadianceMaterial, 3u> palette{
                {{vacuum, 0u}, {wall, 1u, {.6f, .6f, .6f}}, {white, 2u}}};
            const auto source = lighting::WorldRadianceSource::capture(
                {ids, palette, {}, 1.0, 32u, 32u, lighting::OpticalBoundary::vacuum, {}});
            PPR_TEST_ASSERT(source.has_value());
            const auto [direct, combined, discovery] = fixture.generateBounce(
                *source, 704u + open, {{0, 0}, {32u, 32u}}, 704u + open);
            // Fixed32x32 support: C1=1600, C2=2304, C3=4096 records;
            // 16 edges per C1/C2 record, one per C3 and64 per receiver.
            PPR_TEST_ASSERT(discovery->statistics().m_queries_completed == 132096u);

            const auto primary = fixture.read<lighting::WorldRadianceCoefficients>(direct->coefficientsBuffer());
            const auto reflected = fixture.read<lighting::WorldRadianceCoefficients>(combined->coefficientsBuffer());
            PPR_TEST_ASSERT(primary[12u * 32u + 8u].m_coefficients[0] == 0.0f);
            PPR_TEST_ASSERT(reflected[12u * 32u + 8u].m_status == 0u);
            corner[open] = double(reflected[12u * 32u + 8u].m_coefficients[0]);

            const auto &primary_row = primary[12u * 32u + 8u];
            const auto &combined_row = reflected[12u * 32u + 8u];
            std::println(
                "WORLD_SINGLE_BOUNCE_CORNER candidate=endpoint_connected state={} revision={} source_instance={} source=32x32 receiver=(8.5,12.5) primary_RGB=({},{},{}) primary_status={} combined_RGB=({},{},{}) combined_status={} discovery_faces={} discovery_queries={} discovery_rows={}",
                open != 0u ? "open" : "sealed", combined->cohort().m_geometry_revision,
                reinterpret_cast<std::uintptr_t>(source->get()), primary_row.m_coefficients[0],
                primary_row.m_coefficients[1], primary_row.m_coefficients[2], primary_row.m_status,
                combined_row.m_coefficients[0], combined_row.m_coefficients[1], combined_row.m_coefficients[2],
                combined_row.m_status, discovery->faceCount(), discovery->statistics().m_queries_completed,
                discovery->statistics().m_rows_written);

            const auto native = nativeCornerBounce_(fixture, *source, palette, 704u + open);
            native_corner[open] = native[0];
        }

        std::println(
            "WORLD_SINGLE_BOUNCE_CORNER_GATE closed={} open={} closed_minus_one_percent_open={}", corner[0], corner[1],
            corner[0] - .01 * corner[1]);

        const double relative_open_error = native_corner[1] > 0.0
                                               ? std::abs(corner[1] - native_corner[1]) / native_corner[1]
                                               : std::numeric_limits<double>::infinity();
        std::println(
            "WORLD_SINGLE_BOUNCE_PAIRED_CONTROL candidate=endpoint_connected native_closed={} native_open={} native_margin={} native_gate={} RC_closed={} RC_open={} RC_margin={} RC_leakage_gate={} relative_open_energy_error={} open_energy_gate={} native_control_only=true world_candidate_qualified=false",
            native_corner[0], native_corner[1], native_corner[0] - .01 * native_corner[1],
            native_corner[1] > 0.0 and native_corner[0] <= .01 * native_corner[1] ? "PASS" : "FAIL", corner[0],
            corner[1], corner[0] - .01 * corner[1],
            corner[1] > 0.0 and corner[0] <= .01 * corner[1] ? "PASS" : "REJECTED", relative_open_error,
            relative_open_error <= .15 ? "PASS" : "REJECTED");
        PPR_TEST_ASSERT(native_corner[1] > 0.0 and native_corner[0] <= .01 * native_corner[1]);

        // Both unchanged physical gates apply to this ONE endpoint candidate;
        // a passing native control alone cannot accept the cascade.
        PPR_TEST_ASSERT(corner[1] > 0.0 and corner[0] <= .01 * corner[1]);
        PPR_TEST_ASSERT(relative_open_error <= .15);
        std::vector<u16> offscreen_ids(64u * 32u);
        for (u32 y = 0u; y < 32u; ++y)
            offscreen_ids[y * 64u + 48u] = 1u;
        const std::array<lighting::WorldRadianceMaterial, 2u> offscreen_palette{
            {{vacuum, 0u}, {white, 1u, {.5f, .5f, .5f}}}};
        const auto offscreen = lighting::WorldRadianceSource::capture(
            {offscreen_ids, offscreen_palette, {}, 1.0, 64u, 32u, lighting::OpticalBoundary::vacuum, {1, 1, 1}});
        PPR_TEST_ASSERT(offscreen.has_value());
        const auto [far_direct, far_combined, far_discovery] = fixture.generateBounce(
            *offscreen, 706u, {{0, 0}, {32u, 32u}}, 706u);
        const auto far_primary = fixture.read<lighting::WorldRadianceCoefficients>(far_direct->coefficientsBuffer());
        const auto far_rows = fixture.read<lighting::WorldRadianceCoefficients>(far_combined->coefficientsBuffer());
        PPR_TEST_ASSERT(far_discovery->conservativeOffscreenFaceCount() > 0u);
        PPR_TEST_ASSERT(far_rows[16u * 32u + 20u].m_status == 0u);
        PPR_TEST_ASSERT(far_rows[16u * 32u + 20u].m_coefficients[0] > far_primary[16u * 32u + 20u].m_coefficients[0]);

        const auto restored = fixture.generate(
            black, 707u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {{0, 0}, {8u, 8u}});
        const lighting::WorldRadianceBounceSettings foreign_settings{
            64ull * 1024u * 1024u, 1024ull * 1024u, 32ull * 1024u * 1024u, 706u, false};
        const auto foreign = fixture.m_pass.prepareBounce(far_discovery, foreign_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not foreign and foreign.error() == std::errc::invalid_argument);
        const auto still_direct = fixture.m_pass.acquire(fixture.m_fence.completed());
        PPR_TEST_ASSERT(still_direct.has_value() and *still_direct == restored);
        const auto far_retained = fixture.read<lighting::WorldRadianceCoefficients>(far_combined->coefficientsBuffer());
        PPR_TEST_ASSERT(std::memcmp(far_rows.data(), far_retained.data(), far_combined->bufferBytes()) == 0);

        const auto [bad_direct, rejected, bad_keys] = fixture.generateBounce(
            colour, 708u, {{0, 0}, {8u, 8u}}, 708u, true);
        PPR_TEST_ASSERT(not rejected);
        const auto pristine = fixture.m_pass.acquire(fixture.m_fence.completed());
        PPR_TEST_ASSERT(pristine.has_value() and *pristine == bad_direct);

        const std::vector<u16> empty_ids(8u * 8u);
        const std::array<lighting::WorldRadianceMaterial, 1u> empty_palette{{{vacuum, 0u}}};
        const auto empty = lighting::WorldRadianceSource::capture(
            {empty_ids, empty_palette, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum, {1, 1, 1}});
        PPR_TEST_ASSERT(empty.has_value());
        const auto [empty_direct, empty_combined, empty_discovery] = fixture.generateBounce(
            *empty, 709u, {{0, 0}, {8u, 8u}}, 709u);
        const auto empty_primary = fixture.read<lighting::WorldRadianceCoefficients>(
            empty_direct->coefficientsBuffer());
        const auto empty_result = fixture.read<lighting::WorldRadianceCoefficients>(
            empty_combined->coefficientsBuffer());
        PPR_TEST_ASSERT(empty_discovery->faceCount() == 0u);
        PPR_TEST_ASSERT(std::memcmp(empty_primary.data(), empty_result.data(), empty_direct->bufferBytes()) == 0);

        const auto compatible_source = room_source({.5f, .5f, .5f}, false);
        const auto [compatible_direct, compatible_combined, compatible_keys] = fixture.generateBounce(
            compatible_source, 710u, {{0, 0}, {8u, 8u}}, 710u);
        const auto install_visual = [&](const float metallic) {
            bool admission_ready = false;
            for (u32 step = 0u; step < 512u and not admission_ready; ++step)
                admission_ready = fixture.m_grid.worldCandidateAdmissionReady(fixture.m_fence.completed());

            PPR_TEST_ASSERT(admission_ready);
            GridStagedFrame stage{};
            stage.m_has_catalog = stage.m_full_refresh = stage.m_reset_caches = true;
            stage.m_cell_size_m = 1.0f;
            stage.m_content_fingerprint = 13u;
            stage.m_boundary_material_id = 0u;
            stage.m_world_radiance_source = compatible_source;
            stage.m_world_radiance_revision = 710u;

            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0xffffffffu;
            visual.m_transport_model = kGridTransportOpaqueV1;
            visual.m_emissive_rgb[0] = visual.m_emissive_rgb[1] = visual.m_emissive_rgb[2] = 1.0f;
            visual.m_emissive_strength = 1.0f;
            visual.m_metallic = metallic;
            stage.m_terrain_visuals.push_back(visual);

            stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridStagedUpload upload{};
            upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
            const auto ids = compatible_source->materialIds();
            for (u32 y = 0u; y < 8u; ++y) {
                for (u32 x = 0u; x < 8u; ++x)
                    upload.m_cell_materials[y * 128u + x] = ids[y * 8u + x];
            }

            stage.m_uploads.push_back(std::move(upload));

            PPR_TEST_ASSERT(not fixture.m_grid.stageWorldCandidate(7u, 11u, stage));
            const auto ready = fixture.m_grid.advanceWorldCandidate(
                fixture.m_rhi->getDevice(), fixture.m_fence.completed());
            PPR_TEST_ASSERT(ready.has_value() and *ready);

            PPR_TEST_ASSERT(not fixture.m_grid.activateWorldCandidate(7u, 11u, 13u));
        };

        // Same ID and sigma/j/S admit direct synthetic fields, but the actual
        // resident visual A=.75 cannot consume a bounce built with A=.5.
        install_visual(.25f);
        PPR_TEST_ASSERT(not fixture.m_grid.setWorldRadianceLighting(compatible_direct));

        PPR_TEST_ASSERT(fixture.m_grid.setWorldRadianceLighting(compatible_combined) == std::errc::invalid_argument);

        install_visual(.5f);
        PPR_TEST_ASSERT(not fixture.m_grid.setWorldRadianceLighting(compatible_combined));

        // One horizontal four-cell source: the first receiver is INSIDE its
        // sole emitting cell. The other centres see midpoint support wedges
        // containing64,26,16 bins respectively (half angles pi/4,atan(1/3),
        // atan(1/5)); none lies near a midpoint tie. The CURRENT page mask
        // retains all256 bins for all four receivers, so baseline1024 becomes
        // exactly256+64+26+16=362, not a naive per-generation256 comparison.
        const std::array<u16, 4u> line_ids{1u, 0u, 0u, 0u};
        const std::array<lighting::WorldRadianceMaterial, 2u> line_palette{{{vacuum, 0u}, {white, 1u}}};
        const auto line_source = lighting::WorldRadianceSource::capture(
            {line_ids, line_palette, {}, 1.0, 4u, 1u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(line_source.has_value());

        lighting::WorldRadianceSupportCounts line_support{};
        const auto [line_direct, line_combined, line_faces] = fixture.generateBounce(
            *line_source, 711u, {{0, 0}, {4u, 1u}}, 711u, false, &line_support);
        PPR_TEST_ASSERT(line_support.m_direct_baseline == 1024u and line_support.m_direct_included == 362u);
        PPR_TEST_ASSERT(line_faces->faceCount() > 0u);
        // The conservative face halo contains its own emitter: all outward
        // bins remain uncertain/included, rather than claiming self-occlusion.
        PPR_TEST_ASSERT(line_support.m_moment_baseline == u64{line_faces->faceCount()} * 128u);
        PPR_TEST_ASSERT(line_support.m_moment_included == line_support.m_moment_baseline);
        PPR_TEST_ASSERT(line_support.m_emitter_bounds == 1u and line_support.m_support_tests > 0u);
        PPR_TEST_ASSERT(line_support.m_projection_quanta == 0u and line_support.m_discovery_quanta > 0u);
        PPR_TEST_ASSERT(
            line_support.m_optimistic_work_frames == line_support.m_projection_quanta +
                                                         line_support.m_discovery_quanta +
                                                         line_support.m_other_bounce_quanta +
                                                         line_support.m_packed_direct_quanta +
                                                         line_support.m_packed_moment_quanta);

        const auto line_primary = fixture.read<lighting::WorldRadianceCoefficients>(line_direct->coefficientsBuffer());
        const auto line_output = fixture.read<lighting::WorldRadianceCoefficients>(line_combined->coefficientsBuffer());
        PPR_TEST_ASSERT(std::memcmp(line_primary.data(), line_output.data(), line_direct->bufferBytes()) == 0);

        std::println(
            "WORLD_SINGLE_BOUNCE zero_albedo_identity=checked normalized_K_colour=checked BeerLambert_no_j_reinjection=checked sealed={} around_corner={} offscreen=checked foreign=checked invalid_key=checked zero_faces=checked material_A_match=checked material_A_mismatch=checked lookup_miss_injection=UNRUN direct_pristine=checked",
            corner[0], corner[1]);
    }

    // ------------------------------------------------------------------
    // committed native cells: partial front-face coverage and homogeneous overfill
    // ------------------------------------------------------------------

    void overlayChecks_(Fixture &fixture) {
        using Overlay = lighting::WorldRadianceOverlayCell;
        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const lighting::OpticalCell hard{{}, {}, {8, 4, 2}, lighting::OpticalKind::opaque};
        Shape shape;
        shape.m_width = 8u;
        shape.m_height = 3u;
        shape.m_ids.assign(24u, 0u);
        shape.m_ids[14u] = 1u;
        shape.m_materials = {{vacuum, 0u}, {hard, 1u, {.125f, .25f, .5f}}};
        std::array<Overlay, 2u> cells{
            Overlay{10u, .75f, {2, 4, 6}, {.5f, .25f, .125f}}, Overlay{12u, .875f, {4, 2, 1}, {.25f, .5f, .75f}}};
        const std::array<u32, 3u> keys{10u * 4u, 12u * 4u, 14u * 4u};
        const std::array<std::array<float, 3u>, 3u> moments{{{10, 8, 4}, {10, 8, 4}, {10, 8, 4}}};
        const std::array<Query, 7u> queries{
            Query{.5f, 1.5f, 1, 0, 0, 3, 0u, 2u}, Query{.5f, 1.5f, 1, 0, 0, 5.5f, 0u, 2u},
            Query{.5f, 1.5f, 1, 0, 0, 7, 1u, 2u}, Query{2.5f, 1.5f, 1, 0, 0, .5f, 0u, 2u},
            Query{2, 1.5f, 1, 0, 0, 1, 0u, 2u}, Query{1, 1.5f, 1, 0, 0, 1, 0u, 2u},
            Query{1, 1.5f, 1, 0, 1, 4.5f, 0u, 2u}};
        OverlayTrace evidence{cells, keys, moments, {}, {}};
        const auto [rows, counts] = traceRows_(fixture, shape, queries, false, false, &evidence);
        PPR_TEST_ASSERT(counts[0] == queries.size() and counts[1] == queries.size() and counts[7] == 0u);
        PPR_TEST_ASSERT(evidence.m_append[0] == 3u and evidence.m_append[1] == 0u);

        auto discovered = evidence.m_discovered;
        std::ranges::sort(discovered);

        PPR_TEST_ASSERT(std::ranges::equal(discovered, keys));

        const auto close = [](const float actual, const double expected) {
            PPR_TEST_ASSERT(
                std::isfinite(actual) and std::abs(double(actual) - expected) <= 1e-5 + 1e-4 * std::abs(expected));
        };

        for (u32 channel = 0u; channel < 3u; ++channel) {
            const double first = .75 * double(cells[0].m_source[channel]);
            const double both = first + .25 * .875 * double(cells[1].m_source[channel]);
            close(rows[0].source[channel], first);
            close(rows[0].transmission[channel], .25);
            close(rows[1].source[channel], both);
            close(rows[1].transmission[channel], .25 * .125);
            close(rows[2].source[channel], both + .25 * .125 * hard.m_surface_emission[channel]);
            close(rows[2].transmission[channel], 0);
            close(rows[3].source[channel], 0);
            close(rows[3].transmission[channel], 1);
            close(rows[4].source[channel], first);
            close(rows[4].transmission[channel], .25);
            close(rows[5].source[channel] + rows[5].transmission[channel] * rows[6].source[channel], both);
            close(rows[5].transmission[channel] * rows[6].transmission[channel], .25 * .125);
        }

        // Exact complete keys feed the existing reflected walker. Primary S
        // must not reappear in secondary transport, and every partial owner is required.
        OverlayTrace reflected{cells, keys, moments, {}, {}};
        const auto [secondary, secondary_counts] = traceRows_(fixture, shape, queries, false, true, &reflected);
        PPR_TEST_ASSERT(secondary_counts[7] == 0u);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            const double expected = .75 * double(cells[0].m_albedo[channel]) * double(moments[0][channel]) +
                                    .25 * .875 * double(cells[1].m_albedo[channel]) * double(moments[1][channel]) +
                                    .25 *
                                        .125 *
                                        double(shape.m_materials[1].m_diffuse_albedo[channel]) *
                                        double(moments[2][channel]);
            close(secondary[2].source[channel], expected);
            close(secondary[2].transmission[channel], 0);
        }

        const std::array<Query, 1u> full_ray{queries[2]};
        const std::array<u32, 2u> missing_keys{keys[0], keys[2]};
        const std::array<std::array<float, 3u>, 2u> missing_moments{moments[0], moments[2]};
        OverlayTrace missing{cells, missing_keys, missing_moments, {}, {}};
        const auto [missing_rows, missing_counts] = traceRows_(fixture, shape, full_ray, false, true, &missing);
        PPR_TEST_ASSERT(missing_rows[0].status != 0u and missing_counts[2] == 1u and missing_counts[7] != 0u);

        OverlayTrace overflow{cells, {}, {}, {}, {}};
        overflow.m_capacity = 1u;
        const auto [overflow_rows, overflow_counts] = traceRows_(fixture, shape, full_ray, false, false, &overflow);
        PPR_TEST_ASSERT(overflow.m_append[1] == 1u and overflow_rows[0].status != 0u and overflow_counts[2] == 1u);

        // Unity fill and terrain priority retain the previous opaque start/event.
        auto unity = cells;
        unity[0].m_coverage = 1;
        OverlayTrace filled{unity, keys, moments, {}, {}};
        const auto [unity_rows, unity_counts] = traceRows_(fixture, shape, queries, false, false, &filled);
        PPR_TEST_ASSERT(unity_counts[7] == 0u);
        const auto [inside_secondary, inside_counts] = traceRows_(fixture, shape, queries, false, true, &filled);
        PPR_TEST_ASSERT(inside_counts[7] == 0u);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            close(unity_rows[3].source[channel], double(cells[0].m_source[channel]));
            close(unity_rows[3].transmission[channel], 0);
            close(inside_secondary[3].source[channel], 0);
            close(inside_secondary[3].transmission[channel], 0);
        }

        shape.m_ids[10u] = 1u;
        OverlayTrace priority{cells, keys, moments, {}, {}};
        const auto [priority_rows, priority_counts] = traceRows_(fixture, shape, full_ray, false, false, &priority);
        PPR_TEST_ASSERT(priority_counts[7] == 0u);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            close(priority_rows[0].source[channel], hard.m_surface_emission[channel]);
            close(priority_rows[0].transmission[channel], 0);
        }

        // Authored absorbing overfill is not clipped: terrain plus f=2 medium
        // contributes sigma=(1,2,4), j=(2,4,8) over exactly one/two metres.
        const lighting::OpticalCell terrain_medium{{.25, .5, 1}, {.5, 1, 2}, {}, lighting::OpticalKind::absorbing};
        shape.m_materials.push_back({terrain_medium, 2u});
        shape.m_ids[10u] = 2u;
        std::array<Overlay, 1u> medium{Overlay{10u, .5f, {4, 2, 1}, {}, {.75f, 1.5f, 3}, {1.5f, 3, 6}}};
        const std::array<Query, 1u> medium_ray{Query{1.5f, 1.5f, 1, 0, 0, 2, 0u, 2u}};
        for (const float metric: {1.0f, 2.0f}) {
            const std::array<u32, 1u> medium_key{10u * 4u};
            const std::array<std::array<float, 3u>, 1u> zero_moment{{{0, 0, 0}}};
            const auto medium_source = lighting::WorldRadianceSource::capture(
                {shape.m_ids, shape.m_materials, {}, double(metric), shape.m_width, shape.m_height,
                    lighting::OpticalBoundary::vacuum, {}, medium});
            PPR_TEST_ASSERT(medium_source.has_value());

            OverlayTrace absorbing{medium, medium_key, zero_moment, {}, {}};
            absorbing.m_cell_metres = metric;
            const auto [actual, aggregate] = checkedSkipRows_(fixture, shape, medium_ray, false, &absorbing);
            PPR_TEST_ASSERT(aggregate[7] == 0u);
            const auto [suppressed, suppressed_counts] = traceRows_(
                fixture, shape, medium_ray, false, true, &absorbing);
            PPR_TEST_ASSERT(suppressed_counts[7] == 0u and suppressed[0].status == 0u);

            for (u32 channel = 0u; channel < 3u; ++channel) {
                const double transmission = std::exp(-double(1u << channel) * double(metric));
                close(actual[0].source[channel], .5 * double(medium[0].m_source[channel]) + (1 - transmission));
                close(actual[0].transmission[channel], .5 * transmission);
                close(suppressed[0].source[channel], 0);
                close(suppressed[0].transmission[channel], .5 * transmission);
            }
        }

        // Native corner ownership: tied exposed partial, sole exposed -Y, then
        // the existing concave HARD -X winner with its own S and complete coverage.
        Shape corner;
        corner.m_width = 4u;
        corner.m_height = 2u;
        corner.m_ids.assign(8u, 0u);
        corner.m_materials = shape.m_materials;
        std::array<Overlay, 1u> corner_cell{Overlay{6u, .5f, {3, 2, 1}, {.5f, .5f, .5f}}};
        const std::array<Query, 1u> tie{Query{1.5f, .5f, 1, 1, 0, 1, 0u, 2u}};
        OverlayTrace tied{corner_cell, {}, {}, {}, {}};
        const auto [tie_rows, tie_counts] = traceRows_(fixture, corner, tie, false, false, &tied);
        PPR_TEST_ASSERT(tie_counts[7] == 0u and tied.m_discovered == (std::vector<u32>{6u * 4u}));
        close(tie_rows[0].transmission[0], .5);

        corner.m_ids[5u] = 1u;
        OverlayTrace sole{corner_cell, {}, {}, {}, {}};
        const std::array<Query, 1u> dominant_x{Query{1, .5f, 2, 1, 0, .75f, 0u, 2u}};
        const auto [sole_rows, sole_counts] = traceRows_(fixture, corner, dominant_x, false, false, &sole);
        PPR_TEST_ASSERT(sole_counts[7] == 0u and sole.m_discovered == (std::vector<u32>{6u * 4u + 2u}));
        close(sole_rows[0].transmission[0], .5);

        corner.m_ids[2u] = 1u;
        OverlayTrace concave{corner_cell, {}, {}, {}, {}};
        const auto [concave_rows, concave_counts] = traceRows_(fixture, corner, tie, false, false, &concave);
        PPR_TEST_ASSERT(concave_counts[7] == 0u and concave.m_discovered == (std::vector<u32>{2u * 4u}));
        close(concave_rows[0].source[0], hard.m_surface_emission[0]);
        close(concave_rows[0].transmission[0], 0);

        corner.m_ids.assign(8u, 0u);
        corner_cell[0].m_cell = 1u;
        OverlayTrace mirrored{corner_cell, {}, {}, {}, {}};
        const std::array<Query, 1u> negative{Query{2.5f, 1.5f, -1, -1, 0, 1, 0u, 2u}};
        const auto [negative_rows, negative_counts] = traceRows_(fixture, corner, negative, false, false, &mirrored);
        PPR_TEST_ASSERT(negative_counts[7] == 0u and mirrored.m_discovered == (std::vector<u32>{1u * 4u + 1u}));
        close(negative_rows[0].transmission[0], .5);

        // A hard-backed exact partial boundary has its OWN virtual face. A
        // positive hard prefix stops first; no offset suppresses the boundary.
        Shape backed;
        backed.m_width = 3u;
        backed.m_height = 1u;
        backed.m_ids = {1u, 0u, 0u};
        const lighting::OpticalCell red{{}, {}, {4, 0, 0}, lighting::OpticalKind::opaque};
        backed.m_materials = {{vacuum, 0u}, {red, 1u, {1, 0, 0}}};
        const std::array<Overlay, 1u> green{Overlay{1u, .25f, {0, 8, 0}, {.5f, 1, 0}}};
        const std::array<u32, 1u> backed_key{4u};
        const std::array<std::array<float, 3u>, 1u> backed_moment{}; // Owned storage; the real bake overwrites it.
        const std::array<Query, 4u> backed_rays{
            Query{1, .5f, 1, 0, 0, .5f, 0u, 2u}, Query{1.5f, .5f, 1, 0, 0, .25f, 0u, 2u},
            Query{.5f, .5f, 1, 0, 0, 1, 0u, 2u}, Query{1, .5f, -1, 0, 0, .5f, 0u, 2u}};
        OverlayTrace backed_evidence{green, backed_key, backed_moment, {}, {}};
        const auto [backed_primary, backed_counts] = traceRows_(
            fixture, backed, backed_rays, false, false, &backed_evidence);
        PPR_TEST_ASSERT(backed_counts[7] == 0u);

        auto backed_keys = backed_evidence.m_discovered;
        std::ranges::sort(backed_keys);

        PPR_TEST_ASSERT(backed_keys == (std::vector<u32>{1u, 4u}));
        close(backed_primary[0].source[0], 0);
        close(backed_primary[0].source[1], 2);
        close(backed_primary[0].transmission[0], .75);
        close(backed_primary[1].source[1], 0);
        close(backed_primary[1].transmission[0], 1);
        close(backed_primary[2].source[0], 4);
        close(backed_primary[2].transmission[0], 0);
        close(backed_primary[3].source[0], 4);
        close(backed_primary[3].transmission[0], 0);

        const std::array<Query, 1u> boundary_only{backed_rays[0]};
        backed_evidence.m_bake_moments = true;
        const auto [backed_secondary, backed_secondary_counts] = traceRows_(
            fixture, backed, boundary_only, false, true, &backed_evidence);
        PPR_TEST_ASSERT(backed_secondary_counts[7] == 0u);
        PPR_TEST_ASSERT(backed_secondary_counts[0] == 129u and backed_secondary_counts[1] == 129u);
        PPR_TEST_ASSERT(backed_evidence.m_baked_moments.size() == 1u);
        const auto &baked = backed_evidence.m_baked_moments[0];
        // The actual256-bin cardinal K sum is (pi/2)*positive cosine /256;
        // all128 outward rays enter the adjacent red hard cell immediately.
        const double expected_moment = 4.0 * std::numbers::pi / (512.0 * std::sin(std::numbers::pi / 256.0));
        close(baked[0], expected_moment);
        close(baked[1], 0);
        close(baked[2], 0);
        close(backed_secondary[0].source[0], .25 * .5 * expected_moment);
        close(backed_secondary[0].source[0], .25 * .5 * double(baked[0]));
        close(backed_secondary[0].source[1], 0);
        close(backed_secondary[0].transmission[0], .75);

        // Public source/pass path owns the same tiny projection through direct,
        // discovery, real face moments and secondary replay; A=0 is exact identity.
        shape.m_ids[10u] = 0u;
        for (auto &row: cells)
            row.m_albedo.fill(0.0f);

        shape.m_materials[1].m_diffuse_albedo.fill(0.0f);
        shape.m_materials[1].m_optics.m_surface_emission = {};
        const auto source = lighting::WorldRadianceSource::capture(
            {shape.m_ids, shape.m_materials, {}, 1.0, shape.m_width, shape.m_height, lighting::OpticalBoundary::vacuum,
                {}, cells});
        PPR_TEST_ASSERT(source.has_value());

        auto duplicate = cells;
        duplicate[1].m_cell = duplicate[0].m_cell;
        const auto duplicate_source = lighting::WorldRadianceSource::capture(
            {shape.m_ids, shape.m_materials, {}, 1.0, shape.m_width, shape.m_height, lighting::OpticalBoundary::vacuum,
                {}, duplicate});
        PPR_TEST_ASSERT(not duplicate_source and duplicate_source.error() == std::errc::invalid_argument);

        auto invalid = cells;
        invalid[0].m_coverage = 1.25f;
        const auto invalid_source = lighting::WorldRadianceSource::capture(
            {shape.m_ids, shape.m_materials, {}, 1.0, shape.m_width, shape.m_height, lighting::OpticalBoundary::vacuum,
                {}, invalid});
        PPR_TEST_ASSERT(not invalid_source and invalid_source.error() == std::errc::invalid_argument);

        const auto [direct, combined, complete] = fixture.generateBounce(*source, 970u, {{0, 0}, {8u, 3u}}, 971u);
        const auto direct_rows = fixture.read<lighting::WorldRadianceCoefficients>(direct->coefficientsBuffer());
        const auto combined_rows = fixture.read<lighting::WorldRadianceCoefficients>(combined->coefficientsBuffer());
        // Receiver(0.5,1.5) has unobstructed rays to the only emitting cells:
        // the two sparse partial rows. Black terrain and black sky cannot fake this signal.
        PPR_TEST_ASSERT(direct_rows[8u].m_status == 0u and direct_rows[8u].m_coefficients[0] > 0.0f);

        PPR_TEST_ASSERT(complete->faceCount() > 3u and complete->faceHitOccurrences() >= complete->faceCount());
        PPR_TEST_ASSERT(direct_rows.size() == combined_rows.size());
        PPR_TEST_ASSERT(std::memcmp(direct_rows.data(), combined_rows.data(), direct->bufferBytes()) == 0);
        std::println(
            "WORLD_MATTER_OVERLAY partial_product=.03125 all_keys=checked missing=reject overflow=reject unity=checked corner=checked midpoint_split=checked overfill_depth1_depth2=checked terrain_priority=checked A0_direct_identity=checked source_rows={} discovery_faces={} occurrences={}",
            cells.size(), complete->faceCount(), complete->faceHitOccurrences());
    }

    // ------------------------------------------------------------------
    // committed GPU rows through immutable optical projection
    // ------------------------------------------------------------------

    std::pair<std::shared_ptr<const lighting::WorldRadianceSource>, GridParticleGpuView> committedMatterChecks_(
        Fixture &fixture, lighting::WorldRadiancePass &matter) {
        struct Hot {
            std::array<float, 16u> m_fields{};
        };
        struct Cold {
            std::array<float, 6u> m_fields{};
        };
        static_assert(sizeof(Hot) == kGridParticleNumericalStride and alignof(Hot) == 4u);
        static_assert(sizeof(Cold) == kGridParticleReadonlyStride and alignof(Cold) == 4u);

        const GridWorldIdentity world{7u, 11u, 13u};
        constexpr u64 terrain_revision = 42u;
        auto live = std::make_shared<std::atomic<u64>>(0u);
        u64 epoch = 1000u, request_epoch = 1100u, frame = 1200u;
        auto &device = fixture.m_rhi->getDevice();
        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const lighting::OpticalCell wall{{}, {}, {8, 4, 2}, lighting::OpticalKind::opaque};
        const std::array<lighting::WorldRadianceMaterial, 5u> particles{{
            {{{}, {}, {2, 4, 6}, lighting::OpticalKind::opaque}, 2u},
            {{{}, {}, {4, 2, 1}, lighting::OpticalKind::opaque}, 3u},
            {{{}, {}, {7, 3, 1}, lighting::OpticalKind::opaque}, 4u},
            {{{}, {}, {4, 2, 6}, lighting::OpticalKind::opaque}, 7u, {.25f, .5f, .75f}},
            {{{}, {}, {8, 6, 2}, lighting::OpticalKind::opaque}, 8u, {.75f, .25f, .5f}},
        }};
        Shape shape;
        shape.m_width = 8u;
        shape.m_height = 3u;
        shape.m_ids.assign(24u, 0u);
        shape.m_ids[14u] = 1u;
        shape.m_materials = {{vacuum, 0u}, {wall, 1u}};

        const auto capture_terrain = [&](const Shape &input,
                                         const std::span<const lighting::WorldRadianceMaterial> palette,
                                         const std::optional<u64> selected_revision = {}) {
            lighting::WorldRadianceInput source{};
            source.m_material_ids = input.m_ids;
            source.m_materials = input.m_materials;
            source.m_cell_metres = 1.0;
            source.m_width = input.m_width;
            source.m_height = input.m_height;
            source.m_exterior = lighting::OpticalBoundary::vacuum;
            source.m_particle_materials = palette;
            source.m_world_identity = world;
            source.m_terrain_revision = selected_revision.value_or(terrain_revision);
            const auto captured = lighting::WorldRadianceSource::capture(source);
            PPR_TEST_ASSERT(captured.has_value());
            return *captured;
        };

        const auto terrain = capture_terrain(shape, particles);
        PPR_TEST_ASSERT(not terrain->matterIdentity());

        const auto make_view = [&](const std::span<const Hot> hot,
                                   const std::span<const GridParticleMetadataRow> metadata_rows, const u32 moving_count,
                                   const std::optional<u64> selected_revision = {}) {
            PPR_TEST_ASSERT(hot.size() >= std::max(1u, moving_count));
            const auto metadata = prepareGridParticleMetadata(device, metadata_rows, moving_count, 1.0, ++epoch);
            PPR_TEST_ASSERT(metadata.has_value());
            std::vector<Cold> cold(std::max(1u, moving_count));
            for (auto &row: cold) {
                row.m_fields[0] = row.m_fields[1] = row.m_fields[2] = 1.0f;
                row.m_fields[3] = row.m_fields[4] = row.m_fields[5] = 1.0f;
            }

            GridParticleGpuView view{};
            view.m_numerical = fixture.buffer(std::max(1u, moving_count) * sizeof(Hot), sizeof(Hot), hot.data());
            view.m_readonly = fixture.buffer(cold.size() * sizeof(Cold), sizeof(Cold), cold.data());
            view.m_metadata = *metadata;
            view.m_live_epoch = live;
            view.m_world = world;
            view.m_device_epoch = epoch;
            view.m_publication = epoch + 100u;
            view.m_terrain_revision = selected_revision.value_or(terrain_revision);
            view.m_joint_tick = epoch * 3u;
            live->store(epoch, std::memory_order_release);
            PPR_TEST_ASSERT(view.current());
            return view;
        };

        std::array<Hot, 3u> hot{};
        for (auto &row: hot)
            row.m_fields[15] = 1.0f;

        hot[0].m_fields[0] = 7.25f;
        hot[0].m_fields[1] = 4.25f;
        hot[1].m_fields[0] = 5.25f;
        hot[1].m_fields[1] = 4.25f;
        hot[2].m_fields[0] = 7.125f;
        hot[2].m_fields[1] = 4.5f;
        auto moved_hot = hot;
        for (auto &row: moved_hot)
            row.m_fields[0] += 1.0f;

        const u32 half_fill = std::bit_cast<u32>(.5f);
        // Physical fills add within each cell; only the successive CELL
        // interval transmissions multiply. These exact sums are .75 and .875.
        const std::array<GridParticleMetadataRow, 6u> metadata{{
            {11u, 1u, 1u, 2u, 0.0f, 0.0f, .0625f, std::bit_cast<u32>(.375f)},
            {12u, 1u, 0xffff'ffffu, 2u, 2.75f, 1.75f, 12.0f, std::bit_cast<u32>(.375f)},
            {13u, 1u, 0u, 3u, 0.0f, 0.0f, .125f, std::bit_cast<u32>(.25f)},
            {14u, 1u, 0xffff'ffffu, 3u, 4.75f, 1.75f, 9.0f, std::bit_cast<u32>(.25f)},
            {15u, 1u, 2u, 3u, 0.0f, 0.0f, .25f, std::bit_cast<u32>(.375f)},
            {16u, 1u, 0xffff'ffffu, 4u, 7.5f, 2.5f, 16.0f, std::bit_cast<u32>(.25f)},
        }};
        std::shared_ptr<const lighting::WorldRadianceGeneration> retained_field;

        const auto project =
            [&](const std::shared_ptr<const lighting::WorldRadianceSource> &base, const GridParticleGpuView &view,
                const bool mutate_after_snapshot = false, const std::error_code expected_error = {},
                const u32 expected_submissions = 0u) -> std::shared_ptr<const lighting::WorldRadianceSource> {
            const auto canceled = matter.cancelMatter(fixture.m_fence.completed());
            PPR_TEST_ASSERT(canceled.has_value() and *canceled);

            const bool grouped = mutate_after_snapshot or
                                 expected_submissions > 0u or
                                 expected_error != std::error_code{};
            const lighting::WorldRadianceMatterSettings settings{
                64ull * 1024u * 1024u, 1024ull * 1024u, 32ull * 1024u * 1024u, ++request_epoch, grouped};
            const auto demand = matter.prepareMatter(base, view, settings, fixture.m_fence.completed());
            if (not demand) {
                PPR_TEST_ASSERT(expected_error != std::error_code{} and demand.error() == expected_error);
                return {};
            }

            PPR_TEST_ASSERT(not demand->m_admission_error and demand->m_batches > 0u);
            PPR_TEST_ASSERT(demand->m_input_rows == view.m_metadata->m_count);
            if (mutate_after_snapshot)
                PPR_TEST_ASSERT(demand->m_batches > 1u);

            if (retained_field) {
                const auto prior = fixture.m_pass.acquire(fixture.m_fence.completed());
                PPR_TEST_ASSERT(prior.has_value() and *prior == retained_field);
            }

            for (u32 batch = 0u; batch < demand->m_batches; ++batch) {
                PPR_TEST_ASSERT(matter.canEncodeMatter());
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not matter.encodeMatter(
                    {device, *encoder, fixture.m_fence.completed()}, ++frame, grouped ? 1000.0f : 0.0f));

                if (batch == 0u) {
                    // A definitely unsubmitted encoder leaves its snapshot
                    // encodable; dropping it must not publish an immutable source.
                    PPR_TEST_ASSERT(not matter.discardUnsubmitted());
                    encoder.setNull();
                    const auto unpublished = matter.acquireMatter(fixture.m_fence.completed());
                    PPR_TEST_ASSERT(not unpublished);

                    PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                    PPR_TEST_ASSERT(not matter.encodeMatter(
                        {device, *encoder, fixture.m_fence.completed()}, ++frame, grouped ? 1000.0f : 0.0f));
                }

                fixture.submit(*encoder, true, true, &matter);
                const auto premature = matter.acquireMatter(kFenceValueNone);
                PPR_TEST_ASSERT(not premature and premature.error() == std::errc::device_or_resource_busy);

                const auto ready = matter.advanceMatter(fixture.m_fence.completed());
                if (not ready) {
                    PPR_TEST_ASSERT(expected_error != std::error_code{} and ready.error() == expected_error);

                    const auto unavailable = matter.acquireMatter(fixture.m_fence.completed());
                    PPR_TEST_ASSERT(not unavailable);

                    const auto drained = matter.cancelMatter(fixture.m_fence.completed());
                    PPR_TEST_ASSERT(drained.has_value() and *drained);
                    return {};
                }

                if (grouped and *ready) {
                    if (expected_submissions > 0u)
                        PPR_TEST_ASSERT(batch + 1u == expected_submissions);

                    // Measured groups preserve the immutable snapshot.
                    PPR_TEST_ASSERT(batch + 1u < demand->m_batches);
                    break;
                }

                PPR_TEST_ASSERT(*ready == (batch + 1u == demand->m_batches));

                if (batch == 0u and mutate_after_snapshot) {
                    // This copy is later on the SAME queue than the accepted
                    // all-row snapshot. Remaining quanta must ignore mutable Hot.
                    auto replacement = fixture.buffer(sizeof(moved_hot), sizeof(Hot), moved_hot.data());
                    rhi::ComPtr<rhi::ICommandEncoder> mutate;
                    PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(mutate.writeRef())));
                    mutate->setBufferState(replacement, rhi::ResourceState::CopySource);
                    mutate->setBufferState(view.m_numerical, rhi::ResourceState::CopyDestination);
                    mutate->copyBuffer(view.m_numerical, 0u, replacement, 0u, sizeof(moved_hot));
                    mutate->setBufferState(view.m_numerical, rhi::ResourceState::ShaderResource);
                    fixture.submit(*mutate, false);

                    live->fetch_add(1u, std::memory_order_release);
                    PPR_TEST_ASSERT(not view.current());
                }
            }

            PPR_TEST_ASSERT(not expected_error);

            const auto acquired = matter.acquireMatter(fixture.m_fence.completed());
            PPR_TEST_ASSERT(acquired.has_value());
            const lighting::WorldRadianceMatterIdentity identity{
                {view.m_world, view.m_publication, view.m_terrain_revision, view.m_joint_tick}, view.m_device_epoch};
            PPR_TEST_ASSERT((*acquired)->matterIdentity() == identity);
            PPR_TEST_ASSERT((*acquired)->matterCompletionFence() == fixture.m_fence.lastSubmitted());
            PPR_TEST_ASSERT((*acquired)->matterCompletionFence() != kFenceValueNone);
            PPR_TEST_ASSERT(fixture.m_fence.completed() >= (*acquired)->matterCompletionFence());
            PPR_TEST_ASSERT(
                (*acquired)->overlayBuffer() and
                (*acquired)->overlayLookupBuffer() and
                (*acquired)->overlayAncestorBuffer());
            return *acquired;
        };

        const auto close = [](const float actual, const double expected) {
            PPR_TEST_ASSERT(
                std::isfinite(actual) and std::abs(double(actual) - expected) <= 1e-5 + 1e-4 * std::abs(expected));
        };

        const auto view_a = make_view(hot, metadata, 3u);
        const auto source_a = project(terrain, view_a, true);
        const auto statistics_a = source_a->matterStatistics();
        PPR_TEST_ASSERT(statistics_a.has_value());
        PPR_TEST_ASSERT(statistics_a->m_input_rows == 6u and statistics_a->m_participating == 6u);
        PPR_TEST_ASSERT(statistics_a->m_overlay_cells == 3u and statistics_a->m_partial_cells == 3u);
        PPR_TEST_ASSERT(
            statistics_a->m_invalid == 0u and statistics_a->m_outside == 0u and statistics_a->m_unsupported == 0u);
        PPR_TEST_ASSERT(source_a->overlayCount() == 3u and source_a->overlayAncestorWords() > 0u);
        PPR_TEST_ASSERT(not source_a->matterComparedIdentity());

        const std::array<Query, 5u> queries{
            Query{.5f, 1.5f, 1, 0, 0, 3, 0u, 2u}, Query{.5f, 1.5f, 1, 0, 0, 5.5f, 0u, 2u},
            Query{.5f, 1.5f, 1, 0, 0, 7, 1u, 2u}, Query{6.5f, 2.5f, 1, 0, 0, 1.25f, 0u, 2u},
            Query{5.5f, 1.5f, 1, 0, 0, 2, 1u, 2u}};
        const std::array<u32, 3u> keys{10u * 4u, 12u * 4u, 14u * 4u};
        const std::array<std::array<float, 3u>, 3u> moments{};
        OverlayTrace evidence{{}, keys, moments, {}, {}};
        const auto [rows_a, counts_a] = checkedSkipRows_(fixture, shape, queries, true, &evidence, source_a);
        PPR_TEST_ASSERT(counts_a[0] == queries.size() and counts_a[1] == queries.size() and counts_a[7] == 0u);

        auto discovered = evidence.m_discovered;
        std::ranges::sort(discovered);

        const std::array<u32, 4u> expected_keys{10u * 4u, 12u * 4u, 14u * 4u, 23u * 4u};
        PPR_TEST_ASSERT(std::ranges::equal(discovered, expected_keys));

        for (u32 channel = 0u; channel < 3u; ++channel) {
            const double first = .75 * particles[0].m_optics.m_surface_emission[channel];
            const double both = first + .25 * .875 * particles[1].m_optics.m_surface_emission[channel];
            close(rows_a[0].source[channel], first);
            close(rows_a[0].transmission[channel], .25);

            close(rows_a[1].source[channel], both);
            close(rows_a[1].transmission[channel], .03125);

            close(rows_a[2].source[channel], both + .03125 * wall.m_surface_emission[channel]);
            close(rows_a[2].transmission[channel], 0);

            close(rows_a[3].source[channel], .25 * particles[2].m_optics.m_surface_emission[channel]);
            close(rows_a[3].transmission[channel], .75);

            close(rows_a[4].source[channel], wall.m_surface_emission[channel]);
            close(rows_a[4].transmission[channel], 0);
        }

        const std::array<Query, 1u> full_ray{queries[2]};
        OverlayTrace complete{{}, keys, moments, {}, {}};
        const auto [secondary, secondary_counts] = traceRows_(
            fixture, shape, full_ray, false, true, &complete, source_a);
        PPR_TEST_ASSERT(secondary_counts[7] == 0u and secondary[0].status == 0u);
        for (const float value: secondary[0].source)
            close(value, 0);

        const std::array<u32, 2u> missing_keys{keys[0], keys[2]};
        const std::array<std::array<float, 3u>, 2u> missing_moments{};
        OverlayTrace missing{{}, missing_keys, missing_moments, {}, {}};
        const auto [missing_rows, missing_counts] = traceRows_(
            fixture, shape, full_ray, false, true, &missing, source_a);
        PPR_TEST_ASSERT(missing_rows[0].status != 0u and missing_counts[2] == 1u and missing_counts[7] != 0u);

        OverlayTrace overflow{{}, {}, {}, {}, {}};
        overflow.m_capacity = 1u;
        const auto [overflow_rows, overflow_counts] = traceRows_(
            fixture, shape, full_ray, false, false, &overflow, source_a);
        PPR_TEST_ASSERT(overflow.m_append[1] == 1u and overflow_rows[0].status != 0u and overflow_counts[2] == 1u);

        lighting::WorldRadianceSupportCounts committed_support{};
        const auto [direct_a, combined_a, faces_a] = fixture.generateBounce(
            source_a, 1300u, {{0, 0}, {8u, 3u}}, 1301u, false, &committed_support);
        const auto coefficients_a = fixture.read<lighting::WorldRadianceCoefficients>(direct_a->coefficientsBuffer());
        const auto combined_rows = fixture.read<lighting::WorldRadianceCoefficients>(combined_a->coefficientsBuffer());
        PPR_TEST_ASSERT(coefficients_a[8u].m_status == 0u and coefficients_a[8u].m_coefficients[0] > 0.0f);
        PPR_TEST_ASSERT(faces_a->faceCount() > 3u and faces_a->faceHitOccurrences() >= faces_a->faceCount());
        PPR_TEST_ASSERT(committed_support.m_projection_quanta > 0u);
        PPR_TEST_ASSERT(committed_support.m_direct_baseline == direct_a->statistics().m_queries_attempted);
        PPR_TEST_ASSERT(committed_support.m_direct_included <= committed_support.m_direct_baseline);
        PPR_TEST_ASSERT(committed_support.m_moment_baseline == u64{faces_a->faceCount()} * 128u);
        PPR_TEST_ASSERT(committed_support.m_moment_included == committed_support.m_moment_baseline);
        PPR_TEST_ASSERT(std::memcmp(coefficients_a.data(), combined_rows.data(), direct_a->bufferBytes()) == 0);
        retained_field = direct_a;

        const std::array<GridParticleMetadataRow, 2u> moved_metadata{{
            {11u, 1u, 1u, 7u, 0.0f, 0.0f, 3.0f, std::bit_cast<u32>(.25f)},
            {13u, 1u, 0u, 8u, 0.0f, 0.0f, 5.0f, std::bit_cast<u32>(.75f)},
        }};
        auto mixed_hot = moved_hot;
        mixed_hot[0].m_fields[0] = 6.75f;
        const auto view_b = make_view(mixed_hot, moved_metadata, 2u);
        const auto source_b = project(terrain, view_b, false, {}, 6u);
        const auto statistics_b = source_b->matterStatistics();
        PPR_TEST_ASSERT(statistics_b.has_value() and statistics_b->m_changed_cells == 4u);
        PPR_TEST_ASSERT(source_b->matterComparedIdentity() == source_a->matterIdentity());
        PPR_TEST_ASSERT(source_b->overlayCount() == 1u and statistics_b->m_hard_cells == 1u);

        const std::array<Query, 1u> mixed_ray{Query{2.5f, 1.5f, 1, 0, 0, 2, 0u, 2u}};
        const std::array<u32, 1u> mixed_key{11u * 4u};
        const std::array<std::array<float, 3u>, 1u> mixed_moment{{{10, 8, 4}}};
        OverlayTrace mixed_primary{{}, mixed_key, mixed_moment, {}, {}};
        const auto [mixed_rows, mixed_counts] = checkedSkipRows_(
            fixture, shape, mixed_ray, true, &mixed_primary, source_b);

        OverlayTrace mixed_reflected{{}, mixed_key, mixed_moment, {}, {}};
        const auto [mixed_secondary, mixed_secondary_counts] = traceRows_(
            fixture, shape, mixed_ray, false, true, &mixed_reflected, source_b);
        PPR_TEST_ASSERT(mixed_counts[7] == 0u and mixed_secondary_counts[7] == 0u);

        const std::array<double, 3u> mixed_source{7, 5, 3};
        const std::array<double, 3u> mixed_bounce{6.25, 2.5, 2.25};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            close(mixed_rows[0].source[channel], mixed_source[channel]);
            close(mixed_rows[0].transmission[channel], 0);
            close(mixed_secondary[0].source[channel], mixed_bounce[channel]);
            close(mixed_secondary[0].transmission[channel], 0);
        }

        const auto retained_rows = fixture.read<lighting::WorldRadianceCoefficients>(direct_a->coefficientsBuffer());
        PPR_TEST_ASSERT(std::memcmp(coefficients_a.data(), retained_rows.data(), direct_a->bufferBytes()) == 0);

        OverlayTrace repeated{{}, keys, moments, {}, {}};
        const auto [unchanged_a, unchanged_counts] = checkedSkipRows_(
            fixture, shape, queries, true, &repeated, source_a);
        PPR_TEST_ASSERT(unchanged_counts[7] == 0u and unchanged_a.size() == rows_a.size());
        PPR_TEST_ASSERT(std::memcmp(unchanged_a.data(), rows_a.data(), rows_a.size() * sizeof(Interval)) == 0);

        const auto direct_b = fixture.generate(
            source_b, 1302u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, false, {{0, 0}, {8u, 3u}});
        PPR_TEST_ASSERT(direct_b->statistics().m_rows_copied == 0u);
        const auto coefficients_b = fixture.read<lighting::WorldRadianceCoefficients>(direct_b->coefficientsBuffer());
        PPR_TEST_ASSERT(std::memcmp(coefficients_a.data(), coefficients_b.data(), direct_a->bufferBytes()) != 0);
        retained_field = direct_b;

        // Reuse B's snapshot and radix timings to sort the frozen records in
        // the first submission. Reduction and lookup/compare finish the overlay.
        const auto repeated_b = project(terrain, make_view(mixed_hot, moved_metadata, 2u), false, {}, 3u);
        PPR_TEST_ASSERT(repeated_b->matterComparedIdentity() == source_b->matterIdentity());
        PPR_TEST_ASSERT(repeated_b->matterStatistics()->m_changed_cells == 0u);
        PPR_TEST_ASSERT(repeated_b->overlayCount() == source_b->overlayCount());
        const auto [repeated_b_rows, repeated_b_counts] = checkedSkipRows_(
            fixture, shape, mixed_ray, true, &mixed_primary, repeated_b);
        PPR_TEST_ASSERT(repeated_b_counts[7] == 0u and repeated_b_rows.size() == mixed_rows.size());
        PPR_TEST_ASSERT(
            std::memcmp(repeated_b_rows.data(), mixed_rows.data(), mixed_rows.size() * sizeof(Interval)) == 0);

        // A same-size invalid snapshot can share its submission with radix.
        // Its sticky error must prevent reduction and publication.
        auto invalid_mixed = mixed_hot;
        invalid_mixed[0].m_fields[0] = std::numeric_limits<float>::quiet_NaN();
        PPR_TEST_ASSERT(not project(
            terrain, make_view(invalid_mixed, moved_metadata, 2u), false,
            std::make_error_code(std::errc::argument_out_of_domain)));

        auto bad_rows = metadata;
        bad_rows[0].m_physical_fill_bits = 0u;
        const auto missing_fill = make_view(hot, bad_rows, 3u);
        PPR_TEST_ASSERT(
            not project(terrain, missing_fill, false, std::make_error_code(std::errc::argument_out_of_domain)));

        auto invalid_hot = hot;
        invalid_hot[0].m_fields[0] = std::numeric_limits<float>::quiet_NaN();
        const auto invalid_view = make_view(invalid_hot, metadata, 3u);
        PPR_TEST_ASSERT(
            not project(terrain, invalid_view, false, std::make_error_code(std::errc::argument_out_of_domain)));

        bad_rows = metadata;
        bad_rows[0].m_material = 9u; // Original particle-only ID omitted from the source palette.
        const auto unsupported = make_view(hot, bad_rows, 3u);
        PPR_TEST_ASSERT(not project(terrain, unsupported, false, std::make_error_code(std::errc::not_supported)));

        bad_rows = metadata;
        bad_rows[0].m_physical_fill_bits = std::bit_cast<u32>(std::numeric_limits<float>::quiet_NaN());
        const auto invalid_fill = prepareGridParticleMetadata(device, bad_rows, 3u, 1.0, epoch + 1u);
        PPR_TEST_ASSERT(not invalid_fill and invalid_fill.error() == std::errc::invalid_argument);

        const lighting::WorldRadianceMatterSettings stale_settings{
            64ull * 1024u * 1024u, 1024ull * 1024u, 32ull * 1024u * 1024u, ++request_epoch, false};
        const auto no_view = matter.prepareMatter(terrain, {}, stale_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not no_view and no_view.error() == std::errc::invalid_argument);

        const auto stale_view = make_view(hot, metadata, 3u);
        auto wrong_world = stale_view;
        ++wrong_world.m_world.m_content_fingerprint;
        const auto mismatched_world = matter.prepareMatter(
            terrain, wrong_world, stale_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not mismatched_world and mismatched_world.error() == std::errc::invalid_argument);

        auto wrong_revision = stale_view;
        wrong_revision.m_terrain_revision = 1300u; // Outer lighting revision is not the Matter terrain counter.
        const auto mismatched_revision = matter.prepareMatter(
            terrain, wrong_revision, stale_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not mismatched_revision and mismatched_revision.error() == std::errc::invalid_argument);

        const auto stale_demand = matter.prepareMatter(
            terrain, stale_view, stale_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(stale_demand.has_value() and not stale_demand->m_admission_error);

        live->fetch_add(1u, std::memory_order_release);
        rhi::ComPtr<rhi::ICommandEncoder> stale_encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(stale_encoder.writeRef())));
        const auto before_stale = fixture.m_fence.lastSubmitted();
        PPR_TEST_ASSERT(
            matter.encodeMatter({device, *stale_encoder, fixture.m_fence.completed()}, ++frame) ==
            std::errc::resource_unavailable_try_again);
        PPR_TEST_ASSERT(fixture.m_fence.lastSubmitted() == before_stale);

        stale_encoder.setNull();
        const auto stale_canceled = matter.cancelMatter(fixture.m_fence.completed());
        PPR_TEST_ASSERT(stale_canceled.has_value() and *stale_canceled);

        Shape medium_shape = shape;
        medium_shape.m_ids.assign(24u, 0u);
        const lighting::OpticalCell medium{{.25, .5, 1}, {.5, 1, 2}, {}, lighting::OpticalKind::absorbing};
        medium_shape.m_materials = {{vacuum, 0u}, {medium, 1u}};
        medium_shape.m_ids[10u] = 1u;
        const std::array<lighting::WorldRadianceMaterial, 2u> medium_particles{{
            {{{.375, .75, 1.5}, {.75, 1.5, 3}, {}, lighting::OpticalKind::absorbing}, 5u},
            {{{}, {}, {4, 2, 1}, lighting::OpticalKind::opaque}, 6u},
        }};

        // This changed optical terrain has its own actual fixture revision.
        constexpr u64 medium_terrain_revision = 43u;
        const auto medium_terrain = capture_terrain(medium_shape, medium_particles, medium_terrain_revision);
        const std::array<GridParticleMetadataRow, 2u> medium_metadata{{
            {21u, 1u, 0u, 5u, 0.0f, 0.0f, .0625f, std::bit_cast<u32>(2.0f)},
            {22u, 1u, 0xffff'ffffu, 6u, 2.75f, 1.75f, 7.0f, half_fill},
        }};
        const std::array<Hot, 1u> medium_hot{hot[1]};
        const auto medium_view = make_view(medium_hot, medium_metadata, 1u, medium_terrain_revision);
        const auto medium_source = project(medium_terrain, medium_view);
        PPR_TEST_ASSERT(medium_source->overlayCount() == 1u);
        const std::array<Query, 2u> medium_rays{
            Query{1.5f, 1.5f, 1, 0, 0, 1, 0u, 2u}, Query{1.5f, 1.5f, 1, 0, 0, 2, 0u, 2u}};
        const std::array<u32, 1u> medium_key{10u * 4u};
        const std::array<std::array<float, 3u>, 1u> zero_moment{};
        OverlayTrace medium_evidence{{}, medium_key, zero_moment, {}, {}};
        const auto [medium_rows, medium_counts] = traceRows_(
            fixture, medium_shape, medium_rays, false, false, &medium_evidence, medium_source);
        PPR_TEST_ASSERT(medium_counts[7] == 0u);
        for (u32 ray = 0u; ray < medium_rays.size(); ++ray) {
            const double metres = ray == 0u ? .5 : 1.0;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const double transmission = std::exp(-double(1u << channel) * metres);
                close(
                    medium_rows[ray].source[channel],
                    .5 * double(medium_particles[1].m_optics.m_surface_emission[channel]) + 1.0 - transmission);
                close(medium_rows[ray].transmission[channel], .5 * transmission);
            }
        }

        // Three interleaved input key classes exercise global radix prefix and
        // scatter, then a257-row equal-cell run crosses sorted positions255/256.
        // N=513 also leaves one live lane in the final item workgroup.
        constexpr u32 multi_group_rows = 513u;
        const u32 fine_fill = std::bit_cast<u32>(1.0f / 512.0f);
        std::vector<Hot> multi_hot(multi_group_rows);
        std::vector<GridParticleMetadataRow> multi_metadata(multi_group_rows);
        for (u32 row = 0u; row < multi_group_rows; ++row) {
            // 257 is invertible modulo513, so this visits every sorted ordinal
            // exactly once while interleaving the three native-cell classes.
            const u32 ordinal = (row * 257u) % multi_group_rows;
            const u32 cell_x = ordinal < 129u ? 2u : ordinal < 386u ? 4u : 7u;
            const u32 cell_y = ordinal < 386u ? 1u : 2u;
            const u32 material = ordinal < 129u ? 2u : ordinal < 386u ? 3u : 4u;
            multi_hot[row].m_fields[0] = static_cast<float>(cell_x) + 3.25f;
            multi_hot[row].m_fields[1] = static_cast<float>(cell_y) + 3.25f;
            multi_hot[row].m_fields[15] = 1.0f;
            multi_metadata[row] = {
                row + 100u, 2u, row, material, 0.0f, 0.0f, (row & 1u) != 0u ? .0625f : 8.0f, fine_fill};
        }

        const auto multi_view = make_view(multi_hot, multi_metadata, multi_group_rows);
        const auto multi_source = project(terrain, multi_view);
        const auto multi_statistics = multi_source->matterStatistics();
        PPR_TEST_ASSERT(multi_statistics.has_value());
        PPR_TEST_ASSERT(
            multi_statistics->m_input_rows == multi_group_rows and
            multi_statistics->m_participating == multi_group_rows);
        PPR_TEST_ASSERT(multi_source->overlayCount() == 3u and multi_statistics->m_partial_cells == 3u);

        OverlayTrace multi_evidence{{}, keys, moments, {}, {}};
        const auto [multi_intervals, multi_counts] = traceRows_(
            fixture, shape, queries, false, false, &multi_evidence, multi_source);
        PPR_TEST_ASSERT(
            multi_counts[0] == queries.size() and multi_counts[1] == queries.size() and multi_counts[7] == 0u);

        auto multi_keys = multi_evidence.m_discovered;
        std::ranges::sort(multi_keys);

        PPR_TEST_ASSERT(std::ranges::equal(multi_keys, expected_keys));

        constexpr double first_coverage = 129.0 / 512.0;
        constexpr double second_coverage = 257.0 / 512.0;
        constexpr double last_coverage = 127.0 / 512.0;
        constexpr double first_transmission = 1.0 - first_coverage;
        constexpr double both_transmission = first_transmission * (1.0 - second_coverage);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            const double first = first_coverage * particles[0].m_optics.m_surface_emission[channel];
            const double both = first + first_transmission *
                                            second_coverage *
                                            particles[1].m_optics.m_surface_emission[channel];
            close(multi_intervals[0].source[channel], first);
            close(multi_intervals[0].transmission[channel], first_transmission);

            close(multi_intervals[1].source[channel], both);
            close(multi_intervals[1].transmission[channel], both_transmission);

            close(multi_intervals[2].source[channel], both + both_transmission * wall.m_surface_emission[channel]);
            close(multi_intervals[2].transmission[channel], 0);

            close(
                multi_intervals[3].source[channel], last_coverage * particles[2].m_optics.m_surface_emission[channel]);
            close(multi_intervals[3].transmission[channel], 1.0 - last_coverage);

            close(multi_intervals[4].source[channel], wall.m_surface_emission[channel]);
            close(multi_intervals[4].transmission[channel], 0);
        }

        const auto empty_view = make_view(hot, {}, 0u);
        const auto empty_source = project(terrain, empty_view);
        PPR_TEST_ASSERT(empty_source->matterIdentity().has_value() and empty_source->overlayCount() == 0u);
        const auto empty_statistics = empty_source->matterStatistics();
        PPR_TEST_ASSERT(empty_statistics.has_value() and empty_statistics->m_input_rows == 0u);

        const auto equivalent = project(terrain, make_view(hot, {}, 0u));
        PPR_TEST_ASSERT(equivalent->matchesProjectedOptics(*empty_source));
        PPR_TEST_ASSERT(not equivalent->matchesProjectedOptics(*source_a));

        // The committed empty projection can reuse certified terrain optics.
        // Installing or removing an actual optical overlay must trace a new version.
        const auto publish_temporal = [&](const std::shared_ptr<const lighting::WorldRadianceSource> &input) {
            lighting::WorldRadianceSettings settings{64u * 1024u * 1024u, 1024u * 1024u, 32u * 1024u * 1024u};
            settings.m_transport = lighting::WorldRadianceTransport::temporal_cascades;
            settings.m_temporal = {2u, 4u, 4u, 1.0f};
            settings.m_measure_gpu = true;
            settings.m_temporal_skip_table = true;
            settings.m_temporal_frame_allowance_ms = 1.0f;

            const auto prepared = fixture.m_pass.prepare(
                input, {7u, 11u, 13u, terrain_revision}, settings, fixture.m_fence.completed());
            PPR_TEST_ASSERT(prepared.has_value() and not prepared->m_admission_error);
            for (u32 page = 0u; fixture.m_pass.pendingBatches() > 0u; ++page) {
                PPR_TEST_ASSERT(page < 128u);
                const auto allowed = fixture.m_pass.budgetTemporalFrame(1.0f);
                PPR_TEST_ASSERT(allowed.has_value() and *allowed);

                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(not fixture.m_pass.encode({device, *encoder, fixture.m_fence.completed()}, page));
                fixture.submit(*encoder, true);
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

                const auto complete = fixture.m_pass.advance(fixture.m_fence.completed());
                PPR_TEST_ASSERT(complete.has_value() and *complete == (fixture.m_pass.pendingBatches() == 0u));
            }

            const auto field = fixture.m_pass.acquire(fixture.m_fence.completed());
            PPR_TEST_ASSERT(field.has_value());
            return *field;
        };

        {
            const auto canonical = publish_temporal(terrain);
            const auto copied = publish_temporal(empty_source);
            PPR_TEST_ASSERT(
                copied->source() == empty_source and copied->transportVersion() == canonical->transportVersion());
            PPR_TEST_ASSERT(
                copied->statistics().m_queries_attempted == 0u and copied->statistics().m_rows_copied != 0u);
            PPR_TEST_ASSERT(copied->completionFence() > empty_source->matterCompletionFence());
            PPR_TEST_ASSERT(copied->currentReceiverWindow().m_counts == (std::array<u32, 2>{8u, 3u}));
            PPR_TEST_ASSERT(copied->updatedProbeWindow().m_counts == (std::array<u32, 2>{}));

            PPR_TEST_ASSERT(
                fixture.read<u32>(canonical->coefficientsBuffer()) == fixture.read<u32>(copied->coefficientsBuffer()));
            PPR_TEST_ASSERT(
                fixture.read<u32>(canonical->probeStateBuffer()) == fixture.read<u32>(copied->probeStateBuffer()));
        }

        {
            const auto populated = publish_temporal(source_a);
            const auto removed = publish_temporal(empty_source);
            PPR_TEST_ASSERT(
                populated->statistics().m_queries_attempted > 0u and removed->statistics().m_queries_attempted > 0u);
            PPR_TEST_ASSERT(populated->transportVersion() != removed->transportVersion());
        }

        {
            const auto previous = publish_temporal(empty_source);
            const auto current = publish_temporal(equivalent);
            PPR_TEST_ASSERT(current->transportVersion() == previous->transportVersion());
        }

        OverlayTrace empty_evidence{{}, {}, {}, {}, {}};
        const auto [empty_rows, empty_counts] = traceRows_(
            fixture, shape, queries, false, false, &empty_evidence, empty_source);
        PPR_TEST_ASSERT(empty_counts[7] == 0u);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            close(empty_rows[0].source[channel], 0);
            close(empty_rows[0].transmission[channel], 1);
            close(empty_rows[4].source[channel], wall.m_surface_emission[channel]);
            close(empty_rows[4].transmission[channel], 0);
        }

        std::println(
            "WORLD_MATTER_GPU input_rows={} overlay_cells={} partial_product=.03125 all_keys=checked "
            "missing=reject overflow=reject moving_gas_sourceRow=checked off_ray=checked immutable_snapshot=checked "
            "changed_cells={} mixed_overfill=checked A0_direct_identity=checked multi_group_rows={} "
            "multi_group_counts=129,257,127 global_prefix=checked empty=checked source_fence={}",
            statistics_a->m_input_rows, source_a->overlayCount(), statistics_b->m_changed_cells, multi_group_rows,
            source_a->matterCompletionFence());

        // Unique never-published input for the existing terminal fault controls.
        // Its pass owner, not this return value, must retain it after UNKNOWN.
        return {terrain, make_view(hot, {}, 0u)};
    }

    // ------------------------------------------------------------------
    // metre-scaled homogeneous L/T: independent analytic segment values
    // ------------------------------------------------------------------

    void homogeneousChecks_(Fixture &fixture) {
        const auto run = [&](const std::array<double, 3> sigma, const std::array<double, 3> emission,
                             const std::array<double, 3> surface, const bool opaque, const double metric,
                             const bool finite_segment_only = false) {
            const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
            const lighting::OpticalCell medium{sigma, emission, {}, lighting::OpticalKind::absorbing};
            const lighting::OpticalCell wall{{}, {}, surface, lighting::OpticalKind::opaque};
            const std::array<lighting::WorldRadianceMaterial, 3> optics{{{vacuum, 0u}, {medium, 1u}, {wall, 2u}}};
            std::vector<u16> labels(8u * 3u, 1u);
            if (opaque) {
                for (u32 y = 0u; y < 3u; ++y)
                    labels[y * 8u + 6u] = 2u;
            }

            const auto admitted = lighting::WorldRadianceSource::capture(
                {labels, optics, {}, metric, 8u, 3u, lighting::OpticalBoundary::vacuum, {2.0, 3.0, 4.0}});
            PPR_TEST_ASSERT(admitted.has_value());
            for (const double bound: (*admitted)->underflowLossBound())
                PPR_TEST_ASSERT(std::isfinite(bound) and bound <= 1e-5);

            std::vector<u32> packed((labels.size() + 1u) / 2u), flags(labels.size(), 0u);
            for (u32 index = 0u; index < labels.size(); ++index)
                packed[index / 2u] |= u32{labels[index]} << (16u * (index % 2u));

            std::array<Material, 3> palette{};
            palette[0].kind = 0u;
            palette[1].kind = 2u;
            palette[2].kind = 1u;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                palette[1].sigma[channel] = static_cast<float>(sigma[channel]);
                palette[1].emission[channel] = static_cast<float>(emission[channel]);
                palette[2].source[channel] = static_cast<float>(surface[channel]);
            }

            Parameters parameters{};
            parameters.width = 8u;
            parameters.height = 3u;
            parameters.palette = 3u;
            parameters.hierarchy_levels = 1u;
            parameters.max_segments = 15u;
            parameters.cell_metres = static_cast<float>(metric);
            parameters.sky[0] = 2;
            parameters.sky[1] = 3;
            parameters.sky[2] = 4;
            const Hierarchy hierarchy{0u, 8u, 3u, 1u};
            std::array<Query, 2> queries{Query{.5f, 1.5f, 1, 0, 0, 2, 0u}, Query{.5f, 1.5f, 1, 0, 0, 16, 1u}};
            if (finite_segment_only)
                queries[1] = queries[0];

            const Dispatch dispatch{0u, 0u, 2u, 0u};
            auto ids = fixture.buffer(packed.size() * sizeof(u32), sizeof(u32), packed.data());
            auto materials = fixture.buffer(sizeof(palette), sizeof(Material), palette.data());
            auto vacuum_flags = fixture.buffer(flags.size() * sizeof(u32), sizeof(u32), flags.data());
            auto metadata = fixture.buffer(sizeof(hierarchy), sizeof(hierarchy), &hierarchy);
            auto rays = fixture.buffer(sizeof(queries), sizeof(Query), queries.data());
            auto output = fixture.buffer(2u * sizeof(Interval), sizeof(Interval));
            auto counts = fixture.buffer(8u * sizeof(u32), sizeof(u32));

            rhi::ComPtr<rhi::IShaderObject> root;
            PPR_TEST_ASSERT(not make_error_code(
                fixture.m_rhi->getDevice().createRootShaderObject(fixture.m_programs[0], root.writeRef())));
            rhi::ShaderCursor cursor{root};
            fixture.constant(cursor, "g_world", &parameters, sizeof(parameters));
            fixture.constant(cursor, "g_dispatch", &dispatch, sizeof(dispatch));
            fixture.bindUnusedBounce(cursor);
            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 7>{
                     std::pair{"g_cell_ids", ids.get()}, {"g_materials", materials.get()},
                     {"g_vacuum", vacuum_flags.get()}, {"g_hierarchy", metadata.get()}, {"g_queries", rays.get()},
                     {"g_current", output.get()}, {"g_counts", counts.get()}})
                fixture.bind(cursor, name, buffer);

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            for (auto *const buffer: {ids.get(), materials.get(), vacuum_flags.get(), metadata.get(), rays.get()})
                encoder->setBufferState(buffer, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(fixture.m_unused_face_keys, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(fixture.m_unused_face_lookup, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(fixture.m_unused_face_moments, rhi::ResourceState::UnorderedAccess);

            encoder->setBufferState(counts, rhi::ResourceState::CopyDestination);
            encoder->clearBuffer(counts, 0u, 8u * sizeof(u32));
            encoder->setBufferState(counts, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);

            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(fixture.m_pipelines[0], root);
            pass->dispatchCompute(1u, 1u, 1u);
            pass->end();
            fixture.submit(*encoder, false);

            const auto actual = fixture.read<Interval>(output);
            const auto work = fixture.read<u32>(counts);
            PPR_TEST_ASSERT(work[0] == 2u and work[1] == 2u and work[2] == 0u);
            for (u32 interval = 0u; interval < 2u; ++interval) {
                PPR_TEST_ASSERT(actual[interval].status == 0u);

                const bool terminal = interval != 0u and not finite_segment_only;
                const double distance = (not terminal ? 2.0 : opaque ? 5.5 : 7.5) * metric;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const double attenuation = std::exp(-sigma[channel] * distance);
                    const double integral = sigma[channel] == 0.0
                                                ? distance
                                                : -std::expm1(-sigma[channel] * distance) / sigma[channel];
                    const double boundary = not terminal ? 0.0 : opaque ? surface[channel] : double(channel + 2u);
                    const double expected = emission[channel] * integral + attenuation * boundary;
                    const double error = std::abs(double(actual[interval].source[channel]) - expected);
                    std::println(
                        "WORLD_HOMOGENEOUS interval={} channel={} sigma={} j={} metres={} opaque={} expected={} actual={} absolute_error={}",
                        interval, channel, sigma[channel], emission[channel], distance, opaque, expected,
                        actual[interval].source[channel], error);
                    PPR_TEST_ASSERT(error <= 1e-5 + 1e-4 * std::abs(expected));

                    const double expected_T = not terminal ? attenuation : 0.0;
                    PPR_TEST_ASSERT(
                        std::abs(double(actual[interval].transmission[channel]) - expected_T) <=
                        1e-5 + 1e-4 * expected_T);
                }
            }
        };

        run({0, 0, 0}, {0, 0, 0}, {}, false, .25);
        run({0, 0, 0}, {.2, .4, .1}, {}, false, .25);
        run({1e-6, 2e-6, 3e-6}, {2e-6, 1e-6, 4e-6}, {}, false, .25);
        run({.1, .3, .7}, {.2, .4, .1}, {3, 5, 2}, true, .25);
        run({1000, 1000, 1000}, {0, 0, 0}, {1e20, 2e20, 3e20}, true, 1.0);

        // Two actual1m GPU intervals: reciprocal underflow must not erase
        // the unit equilibrium of a finite, very thick emitting medium.
        const double extreme = std::ldexp(1.0, 127);
        run({extreme, extreme, extreme}, {extreme, extreme, extreme}, {}, false, .5, true);

        // Unequal significands and exponents must retain the analytic
        // equilibria1,.75,2.4; equality of uploaded j/sigma is not a shortcut.
        const double half_extreme = std::ldexp(1.0, 126);
        run({extreme, extreme, 1.25 * half_extreme}, {extreme, 1.5 * half_extreme, 1.5 * extreme}, {}, false, .5, true);

        const lighting::OpticalCell invalid{
            {std::numeric_limits<double>::quiet_NaN(), 0, 0}, {}, {}, lighting::OpticalKind::absorbing};
        const std::array<lighting::WorldRadianceMaterial, 1> invalid_palette{{{invalid, 1u}}};
        const std::array<u16, 1> invalid_ids{1u};
        const auto rejected = lighting::WorldRadianceSource::capture(
            {invalid_ids, invalid_palette, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(not rejected);
    }

    // ------------------------------------------------------------------
    // one shared GPU fixture: analytic invariants then unchanged aperture gate
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // temporal world field publication and feedback sequence
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (temporal_world_field_sequence) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        checkRebuiltSkipRays_(fixture);

        // Only a terminal escape samples the environment. The only opening
        // in this cell enclosure is to the left, and a short finite ray
        // inside it must retain unit transmittance and zero emission.
        {
            Shape enclosure{};
            enclosure.m_width = enclosure.m_height = 8u;
            enclosure.m_ids.assign(64u, 1u);
            enclosure.m_materials = {
                {{{}, {}, {}, lighting::OpticalKind::vacuum}, 0u}, {{{}, {}, {}, lighting::OpticalKind::opaque}, 1u}};
            for (u32 x = 0u; x <= 4u; ++x)
                enclosure.m_ids[4u * 8u + x] = 0u;

            const std::array<Query, 3> rays{
                {{4.5f, 4.5f, -1, 0, 0, 16, 1}, {4.5f, 4.5f, 1, 0, 0, 16, 1}, {4.5f, 4.5f, -1, 0, 0, 1, 0}}};
            const auto [rows, counts] = traceRows_(fixture, enclosure, rays, true, false, nullptr, {}, true, 13u);
            PPR_TEST_ASSERT(counts[7] == 0u);
            for (u32 ch = 0; ch < 3u; ++ch) {
                PPR_TEST_ASSERT(rows[0].source[ch] > 0.0f and rows[0].transmission[ch] == 0.0f);
                PPR_TEST_ASSERT(rows[1].source[ch] == 0.0f and rows[1].transmission[ch] == 0.0f);
                PPR_TEST_ASSERT(rows[2].source[ch] == 0.0f and rows[2].transmission[ch] == 1.0f);
            }
        }


        lighting::WorldRadianceSettings settings{64u * 1024u * 1024u, 1024u * 1024u, 32u * 1024u * 1024u};
        settings.m_transport = lighting::WorldRadianceTransport::temporal_cascades;
        settings.m_temporal = {2u, 4u, 4u, 1.0f};
        settings.m_diagnostic_coefficients = true;
        settings.m_measure_gpu = true;
        settings.m_temporal_query_budget = 131072u;
        std::array<u16, 64u> ids{};
        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        const std::array<lighting::WorldRadianceMaterial, 1u> palette{{{vacuum, 0u}}};
        const auto capture = [&](const double sky) {
            auto result = lighting::WorldRadianceSource::capture(
                {ids, palette, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum, {sky, sky, sky}});
            PPR_TEST_ASSERT(result.has_value());
            return *result;
        };

        u64 revision = 1u, world = 11u, job = 7u;
        u64 sample_time_ns = (1ull << 40u) - 16'766'667u;
        u64 sample_interval_ns = 16'666'667u;
        bool yield_refresh{};
        const auto generate = [&](const std::shared_ptr<const lighting::WorldRadianceSource> &source,
                                  lighting::WorldRadiancePass *const control = nullptr) {
            auto &owner = control ? *control : fixture.m_pass;
            auto selected_settings = settings;
            selected_settings.m_temporal_serial_reference = control != nullptr;
            selected_settings.m_temporal_gpu_queue = control == nullptr;
            if (not control)
                sample_time_ns += sample_interval_ns;
            selected_settings.m_temporal_sample_time_ns = sample_time_ns;
            selected_settings.m_temporal_skip_table = control == nullptr;
            const auto admitted = owner.prepare(
                source, {job, world, 13u, revision++}, selected_settings, fixture.m_fence.completed());
            PPR_TEST_ASSERT(admitted.has_value() and not admitted->m_admission_error);
            const u32 batches = owner.pendingBatches();
            PPR_TEST_ASSERT(batches > 0u);
            if (source->width() == 8u)
                PPR_TEST_ASSERT(batches == 1u);
            else
                PPR_TEST_ASSERT(batches > 4u);
            for (u32 phase = 0u; owner.pendingBatches() > 0u; ++phase) {
                PPR_TEST_ASSERT(phase < batches);
                if (selected_settings.m_temporal_frame_allowance_ms > 0.0f) {
                    const u32 pending = owner.pendingBatches();
                    const auto deferred = owner.budgetTemporalFrame(0.0f);
                    PPR_TEST_ASSERT(deferred.has_value() and not *deferred);
                    PPR_TEST_ASSERT(owner.pendingBatches() == pending);

                    const auto invalid_budget = owner.budgetTemporalFrame(-1.0f);
                    PPR_TEST_ASSERT(not invalid_budget and invalid_budget.error() == std::errc::invalid_argument);
                    const bool allow_refresh = not yield_refresh or phase < 2u;
                    const auto budgeted = owner.budgetTemporalFrame(phase % 2u == 0u ? 0.5f : 1.0f, allow_refresh);
                    PPR_TEST_ASSERT(budgeted.has_value() and *budgeted);
                    PPR_TEST_ASSERT(
                        allow_refresh ? owner.pendingBatches() == pending : owner.pendingBatches() <= pending);
                }

                if (phase == 0u) {
                    rhi::ComPtr<rhi::ICommandEncoder> dropped;
                    PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(dropped.writeRef())));
                    PPR_TEST_ASSERT(
                        not owner.encode({fixture.m_rhi->getDevice(), *dropped, fixture.m_fence.completed()}, phase));
                    PPR_TEST_ASSERT(not owner.discardUnsubmitted());
                    PPR_TEST_ASSERT(owner.pendingBatches() == batches);
                }

                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                PPR_TEST_ASSERT(
                    not owner.encode({fixture.m_rhi->getDevice(), *encoder, fixture.m_fence.completed()}, phase));
                fixture.submit(*encoder, true, true, &owner);
                if (selected_settings.m_temporal_frame_allowance_ms > 0.0f) {
                    const auto submitted_budget = owner.budgetTemporalFrame(1.0f);
                    PPR_TEST_ASSERT(
                        not submitted_budget and submitted_budget.error() == std::errc::operation_not_permitted);
                }
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
                const auto ready = owner.advance(fixture.m_fence.completed());
                PPR_TEST_ASSERT(ready.has_value() and *ready == (owner.pendingBatches() == 0u));
                if (settings.m_temporal_frame_allowance_ms == 0.0f)
                    PPR_TEST_ASSERT(owner.pendingBatches() == batches - phase - 1u);
            }

            const auto field = owner.acquire(fixture.m_fence.completed());
            PPR_TEST_ASSERT(field.has_value());
            const auto rows = owner.readDiagnosticCoefficients(fixture.m_fence.completed());
            PPR_TEST_ASSERT(rows.has_value());
            const auto profile = lighting::planWorldRadianceTemporalProfile(
                source->width(), source->height(), settings.m_temporal);
            PPR_TEST_ASSERT(
                profile.has_value() and
                rows->size() == u64{profile->m_refined_counts[0]} * profile->m_refined_counts[1]);
            const auto states = owner.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(states.has_value() and states->size() == rows->size());
            const auto updated = (*field)->updatedProbeWindow();
            const auto minimum = (*field)->probeMinimum();
            const auto counts = (*field)->probeCounts();
            if (settings.m_receiver_window.m_counts == (std::array<u32, 2>{})) {
                const auto covered = (*field)->currentReceiverWindow();
                PPR_TEST_ASSERT(covered.m_minimum == (std::array<i32, 2>{0, 0}));
                PPR_TEST_ASSERT(covered.m_counts == (std::array<u32, 2>{source->width(), source->height()}));
            }
            for (u32 y = 0u; y < updated.m_counts[1]; ++y) {
                for (u32 x = 0u; x < updated.m_counts[0]; ++x) {
                    const u32 row = (static_cast<u32>(updated.m_minimum[1] - minimum[1]) + y) * counts[0] +
                                    static_cast<u32>(updated.m_minimum[0] - minimum[0]) +
                                    x;
                    const auto &state = (*states)[row];
                    const u64 version = u64{state.m_source_version[0]} | (u64{state.m_source_version[1]} << 32u);
                    const u64 sample_time = u64{state.m_sample_time_ns[0]} | (u64{state.m_sample_time_ns[1]} << 32u);
                    PPR_TEST_ASSERT(version == (*field)->transportVersion());
                    PPR_TEST_ASSERT(sample_time == selected_settings.m_temporal_sample_time_ns);
                    PPR_TEST_ASSERT(
                        state.m_samples > 0u and std::isfinite(state.m_variation) and state.m_variation >= 0.0f);
                }
            }
            return std::pair{*field, *rows};
        };

        std::shared_ptr<const lighting::WorldRadianceGameplayField> retained_gameplay;
        const auto black = capture(0.0);
        auto initial = generate(black);
        for (const auto &row: initial.second) {
            for (const float value: row.m_coefficients)
                PPR_TEST_ASSERT(value == 0.0f);
        }

        // A quiet probe retains a quarter of its initial variation after two
        // display intervals, even when the nanosecond clock's low word wraps.
        sample_interval_ns = 33'333'333u;
        std::ignore = generate(black);
        const auto quiet = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
        PPR_TEST_ASSERT(quiet and not quiet->empty());
        PPR_TEST_ASSERT(std::abs(quiet->front().m_variation - 0.25f) < 0.0001f);

        sample_interval_ns = 0u;
        std::ignore = generate(black);
        const auto repeated = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
        PPR_TEST_ASSERT(repeated and repeated->front().m_variation == quiet->front().m_variation);
        auto earlier = settings;
        earlier.m_temporal_sample_time_ns = sample_time_ns - 1u;
        const auto reversed = fixture.m_pass.prepare(
            black, {7u, world, 13u, revision}, earlier, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not reversed and reversed.error() == std::errc::invalid_argument);

        sample_interval_ns = 16'666'667u;

        // A converged, current dark field can skip refinement immediately.
        // Its age bound and a new optical source independently wake the work.
        {
            settings.m_prioritize_convergence = true;
            sample_interval_ns = 0u;
            auto deferred = generate(black);
            const auto deferred_states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(deferred_states and deferred_states->front().m_samples == repeated->front().m_samples);
            PPR_TEST_ASSERT(deferred.first->statistics().m_rows_copied == deferred_states->size());

            sample_interval_ns = 250'000'000u;
            const auto quiet_queries = deferred.first->statistics().m_queries_attempted;
            deferred.first.reset();
            auto aged = generate(black);
            const auto aged_states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(aged_states and aged_states->front().m_samples == repeated->front().m_samples + 1u);
            PPR_TEST_ASSERT(aged.first->statistics().m_rows_copied == 0u);
            PPR_TEST_ASSERT(quiet_queries < aged.first->statistics().m_queries_attempted);
            std::println(
                "WORLD_TEMPORAL_QUIET_CONTROL quiet_queries={} aged_queries={} copied_probes={}", quiet_queries,
                aged.first->statistics().m_queries_attempted, repeated->size());

            const auto aged_version = aged.first->transportVersion();
            aged.first.reset();
            const auto changed = generate(capture(0.5));
            PPR_TEST_ASSERT(changed.first->transportVersion() != aged_version);
            PPR_TEST_ASSERT(changed.first->statistics().m_rows_copied == 0u);
            settings.m_prioritize_convergence = false;
            sample_interval_ns = 16'666'667u;
        }

        // A retained presentation lease must stay bit-identical while subsequent
        // cycles update their own slots and retain previous-field feedback.
        const auto sky = capture(1.0);
        double lit{};
        for (u32 cycle = 0u; cycle < 8u; ++cycle) {
            const auto result = generate(sky);
            const auto states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(states.has_value());
            for (const auto &state: *states)
                PPR_TEST_ASSERT(state.m_samples == cycle + 1u);

            lit = 0.0;
            for (const auto &row: result.second) {
                lit += double{row.m_coefficients[0]};
                if (cycle == 0u)
                    PPR_TEST_ASSERT(std::abs(row.m_coefficients[0] - 1.0f) <= 1e-4f);
                if (cycle == 7u)
                    PPR_TEST_ASSERT(row.m_coefficients[0] > 0.8f and row.m_coefficients[0] <= 1.01f);
            }
        }
        PPR_TEST_ASSERT(lit > 1.0);
        const auto retained = fixture.read<lighting::WorldRadianceCoefficients>(initial.first->coefficientsBuffer());
        PPR_TEST_ASSERT(
            std::memcmp(retained.data(), initial.second.data(), retained.size() * sizeof(retained[0])) == 0);
        initial.first.reset();

        double dark{};
        for (u32 cycle = 0u; cycle < 12u; ++cycle) {
            const auto result = generate(black);
            dark = 0.0;
            for (const auto &row: result.second) {
                if (cycle == 0u)
                    PPR_TEST_ASSERT(row.m_coefficients[0] == 0.0f);
                dark += double{row.m_coefficients[0]};
            }
        }
        PPR_TEST_ASSERT(dark >= 0.0 and dark < lit * 0.05);

        // Changing the hour on the identical immutable source invalidates the
        // optical version while retaining the lattice. A repeated hour can converge.
        {
            ++world;
            settings.m_temporal.m_sky_hour = 9;
            const auto morning = generate(black);
            PPR_TEST_ASSERT(morning.first->skyHour() == 9 and morning.second[0].m_coefficients[0] > 0.0f);
            const auto morning_version = morning.first->transportVersion();
            {
                const auto repeat = generate(black);
                PPR_TEST_ASSERT(repeat.first->transportVersion() == morning_version);
            }

            settings.m_temporal.m_sky_hour = 15;
            {
                const auto afternoon = generate(black);
                PPR_TEST_ASSERT(afternoon.first->transportVersion() != morning_version);
                PPR_TEST_ASSERT(afternoon.second[0].m_coefficients[3] * morning.second[0].m_coefficients[3] < 0.0f);
            }

            settings.m_temporal.m_sky_hour = 0;
            {
                const auto midnight = generate(black);
                for (const auto &row: midnight.second)
                    PPR_TEST_ASSERT(row.m_coefficients[0] == 0.0f and row.m_status == 0u);
            }
        }
        // Inspect a real enclosed refined probe as well as individual wall rays.
        // A vacuum cavity is surrounded by nonemissive, nonreflecting material.
        ++world;
        std::array<u16, 64u> shell;
        shell.fill(1u);
        for (u32 y = 2u; y < 6u; ++y) {
            for (u32 x = 2u; x < 6u; ++x)
                shell[8u * y + x] = 0u;
        }

        const std::array<lighting::WorldRadianceMaterial, 2u> shell_materials{
            {{vacuum, 0u}, {{{}, {}, {}, lighting::OpticalKind::opaque}, 1u, {}}}};
        const auto shell_source = lighting::WorldRadianceSource::capture(
            {shell, shell_materials, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(shell_source.has_value());
        settings.m_temporal.m_sky_hour = 12;
        {
            const auto enclosed = generate(*shell_source);
            const auto minimum = enclosed.first->probeMinimum();
            const auto row = static_cast<u32>(2 - minimum[1]) * enclosed.first->probeCounts()[0] +
                             static_cast<u32>(2 - minimum[0]);
            for (const float coefficient: enclosed.second[row].m_coefficients)
                PPR_TEST_ASSERT(coefficient == 0.0f);
        }

        settings.m_temporal.m_sky_hour = -1;

        // Reuse the same device and field owner for emissive geometry and
        // coloured, nearly white-albedo feedback. No analytic light list.
        std::array<lighting::WorldRadianceMaterial, 3u> room_palette{
            {{vacuum, 0u}, {{{}, {}, {}, lighting::OpticalKind::opaque}, 1u, {}},
                {{{}, {}, {2.0, 2.0, 2.0}, lighting::OpticalKind::opaque}, 2u, {}}}};
        for (u32 y = 0u; y < 8u; ++y) {
            for (u32 x = 0u; x < 8u; ++x)
                ids[y * 8u + x] = x == 0u or y == 0u or x == 7u or y == 7u ? 1u : 0u;
        }
        ids[9u] = 2u;
        const auto room = [&](const std::span<const lighting::WorldRadianceOverlayCell> overlay = {}) {
            const auto result = lighting::WorldRadianceSource::capture(
                {ids, room_palette, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum, {}, overlay});
            PPR_TEST_ASSERT(result.has_value());
            return *result;
        };
        const auto energy = [](const auto &rows) {
            std::array<double, 3u> result{};
            for (const auto &row: rows) {
                for (u32 channel = 0u; channel < 3u; ++channel)
                    result[channel] += double{row.m_coefficients[channel]};
            }
            return result;
        };

        ++world; // Both serial and parallel controls start with black, incompatible history.
        auto source = room();
        std::array<double, 3u> direct{};
        for (u32 cycle = 0u; cycle < 8u; ++cycle) {
            const auto parallel = generate(source);
            const auto serial = generate(source, &fixture.m_matter_pass);
            PPR_TEST_ASSERT(parallel.second.size() == serial.second.size());
            PPR_TEST_ASSERT(
                std::memcmp(
                    parallel.second.data(), serial.second.data(),
                    parallel.second.size() * sizeof(parallel.second[0])) == 0);
            direct = energy(parallel.second);
        }
        PPR_TEST_ASSERT(direct[0] > 0.0);

        const auto checked = [&](const std::shared_ptr<const lighting::WorldRadianceSource> &input) {
            auto result = generate(input);
            const auto control = generate(input, &fixture.m_matter_pass);
            PPR_TEST_ASSERT(result.second.size() == control.second.size());
            PPR_TEST_ASSERT(
                std::memcmp(
                    result.second.data(), control.second.data(), result.second.size() * sizeof(result.second[0])) == 0);
            return result;
        };

        room_palette[1].m_diffuse_albedo = {0.99f, 0.3f, 0.1f};
        source = room();
        {
            const auto started = checked(source);
            sample_interval_ns = 0u;
            const auto repeated_field = checked(source);
            PPR_TEST_ASSERT(started.second.size() == repeated_field.second.size());
            PPR_TEST_ASSERT(
                std::memcmp(
                    started.second.data(), repeated_field.second.data(),
                    started.second.size() * sizeof(started.second[0])) == 0);
        }

        sample_interval_ns = 16'666'667u;
        std::array<double, 3u> bounced{};
        for (u32 cycle = 0u; cycle < 16u; ++cycle)
            bounced = energy(checked(source).second);
        PPR_TEST_ASSERT(bounced[0] > direct[0] and bounced[0] > bounced[1] and bounced[1] > bounced[2]);
        PPR_TEST_ASSERT(bounced[0] < 100.0); // Scene-relative finite-energy ceiling, not a product quality bound.

        const auto before_move = checked(source).second;
        ids[9u] = 0u;
        ids[54u] = 2u;
        source = room();
        std::vector<lighting::WorldRadianceCoefficients> moved;
        for (u32 cycle = 0u; cycle < 6u; ++cycle)
            moved = checked(source).second;
        PPR_TEST_ASSERT(std::memcmp(before_move.data(), moved.data(), moved.size() * sizeof(moved[0])) != 0);

        for (u32 y = 1u; y < 7u; ++y)
            ids[y * 8u + 4u] = 1u;
        source = room();
        for (u32 cycle = 0u; cycle < 6u; ++cycle)
            moved = checked(source).second;
        const auto closed = moved;
        ids[4u * 8u + 4u] = 0u;
        const lighting::WorldRadianceOverlayCell partial{27u, 0.5f, {}, {0.7f, 0.2f, 0.1f}, {}, {}};
        source = room(std::span{&partial, 1u});
        for (u32 cycle = 0u; cycle < 6u; ++cycle)
            moved = checked(source).second;
        PPR_TEST_ASSERT(std::memcmp(closed.data(), moved.data(), moved.size() * sizeof(moved[0])) != 0);

        const auto before_off = energy(moved);
        room_palette[2].m_optics.m_surface_emission = {};
        source = room();
        for (u32 cycle = 0u; cycle < 32u; ++cycle)
            moved = checked(source).second;
        const auto after_off = energy(moved);
        PPR_TEST_ASSERT(after_off[0] >= 0.0 and after_off[0] < before_off[0] * 0.9);

        // A blue-only source change makes some rows vary while untouched dark
        // rows remain quiet. Compare the exact per-row selection and history.
        room_palette[2].m_optics.m_surface_emission = {0.0, 0.0, 2.0};
        source = room();
        {
            const auto before = generate(source);
            const auto history = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(history.has_value());

            settings.m_prioritize_convergence = true;
            sample_interval_ns = 30'000'000u;
            const auto after = generate(source);
            const auto states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(states and states->size() == history->size());

            u32 active{};
            for (std::size_t row = 0u; row < states->size(); ++row) {
                const float change = std::min(1.0f, std::sqrt((*history)[row].m_variation));
                const bool due = 30.0f * (1.0f + 14.0f * change) >= 250.0f;
                if (due) {
                    PPR_TEST_ASSERT((*states)[row].m_samples == (*history)[row].m_samples + 1u);
                    ++active;
                } else {
                    PPR_TEST_ASSERT(std::memcmp(&(*states)[row], &(*history)[row], sizeof((*states)[row])) == 0);
                    PPR_TEST_ASSERT(
                        std::memcmp(&after.second[row], &before.second[row], sizeof(after.second[row])) == 0);
                }
            }
            PPR_TEST_ASSERT(active > 0u and active < states->size());
            PPR_TEST_ASSERT(after.first->statistics().m_rows_copied == states->size() - active);
            std::println(
                "WORLD_TEMPORAL_CHROMATIC_PRIORITY updated={} retained={} elapsed_ms=30", active,
                states->size() - active);
            settings.m_prioritize_convergence = false;
            sample_interval_ns = 16'666'667u;
        }

        // Exercise 16/32-cell skips and the per-resource (not aggregate-pool)
        // cap on the same fixture, with a narrow aperture and partial RGB media.
        ++world;
        settings.m_temporal = {4u, 6u, 4u, 1.0f};
        settings.m_max_resource_bytes = 1536u * 1024u;
        settings.m_temporal_query_budget = 32768u;
        settings.m_temporal_frame_allowance_ms = 1.0f;
        room_palette[2].m_optics.m_surface_emission = {2.0, 1.0, 0.5};
        std::array<u16, 64u * 64u> large_ids{};
        const auto large = [&](const std::span<const lighting::WorldRadianceOverlayCell> overlays = {}) {
            const auto result = lighting::WorldRadianceSource::capture(
                {large_ids, room_palette, {}, 1.0, 64u, 64u, lighting::OpticalBoundary::vacuum, {0.3, 0.2, 0.1},
                    overlays});
            PPR_TEST_ASSERT(result.has_value());
            return *result;
        };
        // A large, one-cell-thick opaque shell straddles multiple cascade
        // intervals. Every connected path to an exterior sky must hit it.
        for (u32 i = 1u; i < 63u; ++i) {
            large_ids[64u + i] = large_ids[62u * 64u + i] = 1u;
            large_ids[64u * i + 1u] = large_ids[64u * i + 62u] = 1u;
        }

        settings.m_temporal.m_sky_hour = 12;
        const auto thin_shell = lighting::WorldRadianceSource::capture(
            {large_ids, shell_materials, {}, 1.0, 64u, 64u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(thin_shell.has_value());
        {
            const auto enclosed = checked(*thin_shell);
            const auto minimum = enclosed.first->probeMinimum();
            for (i32 y = 2; y < 14; ++y) {
                for (i32 x = 2; x < 14; ++x) {
                    const auto row = static_cast<u32>(y - minimum[1]) * enclosed.first->probeCounts()[0] +
                                     static_cast<u32>(x - minimum[0]);
                    PPR_TEST_ASSERT(enclosed.second[row].m_coefficients[0] == 0.0f);
                }
            }
        }

        large_ids.fill(0u);
        settings.m_temporal.m_sky_hour = -1;
        ++world;
        source = large();
        checked(source);
        for (u32 y = 0u; y < 64u; ++y)
            large_ids[y * 64u + 31u] = 1u;
        large_ids[32u * 64u + 31u] = 0u;
        large_ids[30u * 64u + 30u] = 2u;
        source = large();
        checked(source);
        const lighting::WorldRadianceOverlayCell medium{
            32u * 64u + 31u, 0.5f, {}, {0.7f, 0.2f, 0.1f}, {0.1f, 0.3f, 0.7f}, {0.02f, 0.01f, 0.03f}};
        source = large(std::span{&medium, 1u});
        checked(source);
        large_ids.fill(0u);
        checked(large());

        {
            const auto before_sparse = checked(large());
            const auto before_states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(before_states.has_value());
            settings.m_receiver_window = {{16, 16}, {8u, 8u}};
            settings.m_temporal_coverage_window = settings.m_receiver_window;
            const auto bright_source = lighting::WorldRadianceSource::capture(
                {large_ids, room_palette, {}, 1.0, 64u, 64u, lighting::OpticalBoundary::vacuum, {1.0, 2.0, 3.0}});
            PPR_TEST_ASSERT(bright_source.has_value());
            // Interrupt a previously accepted window at its first page fence.
            // The replacement must still inherit every untouched published row.
            auto interrupted = settings;
            interrupted.m_temporal_sample_time_ns = sample_time_ns;
            const auto previous_fence = fixture.m_fence.completed();
            const auto admitted = fixture.m_pass.prepare(
                large(), {job, world, 13u, revision++}, interrupted, previous_fence);
            PPR_TEST_ASSERT(admitted.has_value() and not admitted->m_admission_error);
            const auto allowed = fixture.m_pass.budgetTemporalFrame(1.0f);
            PPR_TEST_ASSERT(allowed.has_value() and *allowed);

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            PPR_TEST_ASSERT(not fixture.m_pass.encode({fixture.m_rhi->getDevice(), *encoder, previous_fence}, 1u));
            fixture.submit(*encoder, true, true, &fixture.m_pass);

            const auto busy = fixture.m_pass.prepare(
                *bright_source, {job, world, 13u, revision}, settings, previous_fence);
            PPR_TEST_ASSERT(not busy and busy.error() == std::errc::device_or_resource_busy);
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

            settings.m_temporal_refresh_window = lighting::WorldRadianceReceiverWindow{{40, 32}, {8u, 8u}};
            {
                yield_refresh = true;
                const auto yielded = checked(large());
                yield_refresh = false;
                PPR_TEST_ASSERT(yielded.first->supplementalRefreshYielded());
                PPR_TEST_ASSERT(yielded.first->currentReceiverWindow() == settings.m_temporal_coverage_window);

                const auto states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
                PPR_TEST_ASSERT(states.has_value());
                for (u32 y = 8u; y < 12u; ++y) {
                    for (u32 x = 10u; x < 14u; ++x) {
                        const auto row = y * 18u + x;
                        PPR_TEST_ASSERT(
                            std::memcmp(&(*states)[row], &(*before_states)[row], sizeof((*states)[row])) == 0);
                        PPR_TEST_ASSERT(
                            std::memcmp(
                                &yielded.second[row], &before_sparse.second[row], sizeof(yielded.second[row])) == 0);
                    }
                }
            }

            settings.m_gameplay_demand = lighting::WorldRadianceGameplayDemand{18u, {{63, 63}, {3u, 3u}}};
            const auto uncertified = fixture.m_pass.prepare(
                *bright_source, {job, world, 13u, revision}, settings, fixture.m_fence.completed());
            PPR_TEST_ASSERT(not uncertified and uncertified.error() == std::errc::invalid_argument);

            settings.m_gameplay_demand = lighting::WorldRadianceGameplayDemand{19u, {{18, 18}, {3u, 3u}}};
            const auto sparse = checked(*bright_source);
            settings.m_temporal_refresh_window.reset();
            PPR_TEST_ASSERT(not sparse.first->supplementalRefreshYielded());
            PPR_TEST_ASSERT(
                sparse.first->currentReceiverWindow().m_minimum == settings.m_temporal_coverage_window.m_minimum);
            PPR_TEST_ASSERT(
                sparse.first->currentReceiverWindow().m_counts == settings.m_temporal_coverage_window.m_counts);
            retained_gameplay = sparse.first->gameplayField();
            PPR_TEST_ASSERT(retained_gameplay and retained_gameplay->m_cohort == sparse.first->cohort());
            PPR_TEST_ASSERT(retained_gameplay->m_completion == sparse.first->completionFence());
            PPR_TEST_ASSERT(retained_gameplay->m_transport_version == sparse.first->transportVersion());
            PPR_TEST_ASSERT(retained_gameplay->m_demand.m_sequence == 19u and retained_gameplay->m_rows.size() == 4u);
            for (u32 y = 0u; y < 2u; ++y) {
                for (u32 x = 0u; x < 2u; ++x) {
                    const auto row = (5u + y) * 18u + 5u + x;
                    PPR_TEST_ASSERT(
                        std::memcmp(
                            &retained_gameplay->m_rows[y * 2u + x], &sparse.second[row], sizeof(sparse.second[row])) ==
                        0);
                }
            }
            settings.m_gameplay_demand.reset();

            const auto sparse_states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(sparse_states.has_value());
            const auto updated = sparse.first->updatedProbeWindow();
            PPR_TEST_ASSERT(updated.m_minimum == (std::array<i32, 2u>{3, 3}));
            PPR_TEST_ASSERT(updated.m_counts == (std::array<u32, 2u>{4u, 4u}));
            for (u32 y = 0u; y < 18u; ++y) {
                for (u32 x = 0u; x < 18u; ++x) {
                    const u32 index = y * 18u + x;
                    const bool demanded = x >= 4u and x < 8u and y >= 4u and y < 8u;
                    const bool refreshed = x >= 10u and x < 14u and y >= 8u and y < 12u;
                    const bool changed = demanded or refreshed;
                    if (changed) {
                        PPR_TEST_ASSERT((*sparse_states)[index].m_samples == 1u);
                        PPR_TEST_ASSERT((*sparse_states)[index].m_variation > 0.0f);
                        for (u32 channel = 0u; channel < 3u; ++channel) {
                            const float expected = static_cast<float>(channel + 1u);
                            PPR_TEST_ASSERT(std::abs(sparse.second[index].m_coefficients[channel] - expected) < 1e-4f);
                        }
                    } else {
                        PPR_TEST_ASSERT(
                            std::memcmp(
                                &(*sparse_states)[index], &(*before_states)[index], sizeof((*sparse_states)[index])) ==
                            0);
                        PPR_TEST_ASSERT(
                            std::memcmp(
                                &sparse.second[index], &before_sparse.second[index], sizeof(sparse.second[index])) ==
                            0);
                    }
                }
            }

            settings.m_receiver_window = {{48, 48}, {8u, 8u}};
            const auto relocated = checked(*bright_source);
            PPR_TEST_ASSERT(relocated.first->transportVersion() == sparse.first->transportVersion());
            PPR_TEST_ASSERT(
                relocated.first->currentReceiverWindow().m_minimum == settings.m_temporal_coverage_window.m_minimum);
            PPR_TEST_ASSERT(
                relocated.first->currentReceiverWindow().m_counts == settings.m_temporal_coverage_window.m_counts);
            PPR_TEST_ASSERT(
                std::memcmp(
                    &relocated.second[5u * 18u + 5u], &sparse.second[5u * 18u + 5u], sizeof(sparse.second[0])) == 0);
        }

        // Nonzero retained history from an older source must not certify the requested view.
        const auto stale = checked(large());
        PPR_TEST_ASSERT(stale.first->currentReceiverWindow().m_counts == (std::array<u32, 2>{}));
        const auto stale_states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
        PPR_TEST_ASSERT(stale_states.has_value());
        const auto &retained_state = (*stale_states)[5u * 18u + 5u];
        const u64 retained_version = u64{retained_state.m_source_version[0]} |
                                     (u64{retained_state.m_source_version[1]} << 32u);
        PPR_TEST_ASSERT(retained_version != 0u and retained_version != stale.first->transportVersion());

        // A new world has no compatible history; untouched output must be black.
        ++world;
        settings.m_receiver_window = {{63, 63}, {1u, 1u}};
        settings.m_temporal_coverage_window = {};
        const auto edge = checked(large());
        PPR_TEST_ASSERT(edge.first->currentReceiverWindow().m_counts == (std::array<u32, 2>{}));
        const auto edge_states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
        PPR_TEST_ASSERT(edge_states.has_value());
        PPR_TEST_ASSERT(edge.first->updatedProbeWindow().m_minimum == (std::array<i32, 2u>{15, 15}));
        PPR_TEST_ASSERT(edge.first->updatedProbeWindow().m_counts == (std::array<u32, 2u>{2u, 2u}));
        for (u32 y = 0u; y < 18u; ++y) {
            for (u32 x = 0u; x < 18u; ++x) {
                const auto &row = edge.second[y * 18u + x];
                if (x >= 16u and y >= 16u) {
                    PPR_TEST_ASSERT(std::abs(row.m_coefficients[0] - 0.3f) < 1e-4f);
                    PPR_TEST_ASSERT(std::abs(row.m_coefficients[1] - 0.2f) < 1e-4f);
                    PPR_TEST_ASSERT(std::abs(row.m_coefficients[2] - 0.1f) < 1e-4f);
                    continue;
                }

                const auto &state = (*edge_states)[y * 18u + x];
                PPR_TEST_ASSERT(state.m_source_version == (std::array<u32, 2>{}));
                PPR_TEST_ASSERT(state.m_sample_time_ns == (std::array<u32, 2>{}));
                PPR_TEST_ASSERT(state.m_samples == 0u and state.m_variation == 0.0f);
                for (const float coefficient: row.m_coefficients)
                    PPR_TEST_ASSERT(coefficient == 0.0f);
            }
        }

        // Identical dimensions can name a different world-space probe lattice.
        // Alternate opposite corners so a wrongly retained donor is observable.
        auto displaced = [&](const double cell_metres) {
            auto shifted = lighting::WorldRadianceSource::capture(
                {large_ids, room_palette, {12.0, -8.0}, cell_metres, 64u, 64u, lighting::OpticalBoundary::vacuum,
                    {0.3, 0.2, 0.1}});
            PPR_TEST_ASSERT(shifted.has_value());
            return *shifted;
        };

        const auto check_unknown = [&](const std::shared_ptr<const lighting::WorldRadianceSource> &input,
                                       const i32 position, const u32 untouched) {
            settings.m_receiver_window = {{position, position}, {1u, 1u}};
            const auto field = checked(input);
            const auto states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(states and (*states)[untouched].m_samples == 0u);
            for (const float coefficient: field.second[untouched].m_coefficients)
                PPR_TEST_ASSERT(coefficient == 0.0f);
        };

        check_unknown(displaced(1.0), 0, 17u * 18u + 17u);
        const auto resized = displaced(0.5);
        check_unknown(resized, 63, 0u);
        ++job;
        check_unknown(resized, 0, 17u * 18u + 17u);

        // Explicit distant gameplay work keeps its own dependency closure. The
        // intervening probe rows remain unknown in a fresh world.
        ++world;
        settings.m_receiver_window = {{0, 0}, {4u, 4u}};
        settings.m_temporal_coverage_window = settings.m_receiver_window;
        settings.m_gameplay_demand = lighting::WorldRadianceGameplayDemand{20u, {{56, 56}, {8u, 8u}}};
        {
            const auto distant = checked(large());
            const auto field = distant.first->gameplayField();
            PPR_TEST_ASSERT(field and field->m_demand.m_sequence == 20u);
            PPR_TEST_ASSERT(distant.first->currentReceiverWindow() == settings.m_receiver_window);
            const auto reading = field->sample({63.0, 63.0});
            PPR_TEST_ASSERT(reading and std::abs(reading->m_coefficients[0] - 0.3f) < 1e-4f);

            const auto states = fixture.m_pass.readDiagnosticProbeStates(fixture.m_fence.completed());
            PPR_TEST_ASSERT(states and (*states)[9u * 18u + 9u].m_samples == 0u);
            PPR_TEST_ASSERT((*states)[15u * 18u + 15u].m_samples > 0u);
        }
        settings.m_gameplay_demand.reset();

        settings.m_receiver_window = {{0, 0}, {0u, 8u}};
        const auto malformed = fixture.m_pass.prepare(
            large(), {7u, world, 13u, revision++}, settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not malformed.has_value() and malformed.error() == std::errc::invalid_argument);

        settings.m_receiver_window = {};
        settings.m_measure_gpu = false;
        const auto untimed = fixture.m_pass.prepare(
            large(), {7u, world, 13u, revision++}, settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not untimed.has_value() and untimed.error() == std::errc::invalid_argument);

        // Later world, lattice and source replacements cannot alter a delivered CPU snapshot.
        const auto sample = retained_gameplay->sample({20.5, 19.5});
        PPR_TEST_ASSERT(sample.has_value());
        for (u32 channel = 0u; channel < 3u; ++channel)
            PPR_TEST_ASSERT(std::abs(sample->m_coefficients[channel] - static_cast<float>(channel + 1u)) < 1e-4f);
        auto gradient = *retained_gameplay;
        const std::array<float, 4u> signed_ac{-4.0f, 8.0f, 1.0f, -2.0f};
        for (std::size_t i = 0u; i < signed_ac.size(); ++i)
            gradient.m_rows[i].m_coefficients[5] = signed_ac[i];
        const auto blended = gradient.sample({20.0, 19.0});
        PPR_TEST_ASSERT(blended and std::abs(blended->m_coefficients[5] - 1.375f) < 1e-5f);

        const auto outside = retained_gameplay->sample({0.0, 0.0});
        PPR_TEST_ASSERT(not outside and outside.error() == std::errc::result_out_of_range);

        worldGridChecks_(fixture, true);
    };

    PPR_UNIT_TEST (native_cell_candidate_shared_analytic_wall_aperture_and_constant_normal_contracts) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        PPR_TEST_ASSERT(fixture.m_pass.prepareDiscoveryPipeline() == std::errc::operation_not_permitted);

        fixture.initialize();

        enteredFaceChecks_(fixture);

        const lighting::OpticalCell vacuum{{}, {}, {}, lighting::OpticalKind::vacuum};
        std::array<u16, 64u> vacuum_ids{};
        const std::array<lighting::WorldRadianceMaterial, 1u> vacuum_palette{{{vacuum, 0u}}};
        const auto unit_source = lighting::WorldRadianceSource::capture(
            {vacuum_ids, vacuum_palette, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum, {1.0, 1.0, 1.0}});
        PPR_TEST_ASSERT(unit_source.has_value());
        vacuum_ids[0] = 65535u;
        PPR_TEST_ASSERT((*unit_source)->materialIds()[0] == 0u); // The source owns the copied ID lattice.
        const lighting::WorldRadianceSettings rejected_budget{1u, 0u, 1u, 4u, 0.5f, true};
        const auto rejected = fixture.m_pass.prepare(
            *unit_source, {7u, 11u, 13u, 1u}, rejected_budget, fixture.m_fence.completed());
        PPR_TEST_ASSERT(rejected.has_value() and rejected->m_admission_error == std::errc::no_buffer_space);

        auto invalid_spacing = rejected_budget;
        invalid_spacing.m_refined_spacing = static_cast<lighting::WorldRadianceRefinedSpacing>(3u);
        const auto rejected_spacing = fixture.m_pass.prepare(
            *unit_source, {7u, 11u, 13u, 1u}, invalid_spacing, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not rejected_spacing and rejected_spacing.error() == std::errc::invalid_argument);

        auto invalid_transport = rejected_budget;
        invalid_transport.m_transport = static_cast<lighting::WorldRadianceTransport>(4u);
        const auto rejected_transport = fixture.m_pass.prepare(
            *unit_source, {7u, 11u, 13u, 1u}, invalid_transport, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not rejected_transport and rejected_transport.error() == std::errc::invalid_argument);

        auto invalid_straight = rejected_budget;
        invalid_straight.m_transport = lighting::WorldRadianceTransport::straight256_reference;
        const auto rejected_direct_spacing = fixture.m_pass.prepare(
            *unit_source, {7u, 11u, 13u, 1u}, invalid_straight, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not rejected_direct_spacing and rejected_direct_spacing.error() == std::errc::invalid_argument);

        invalid_straight.m_refined_spacing = lighting::WorldRadianceRefinedSpacing::native_cell_centres;
        invalid_straight.m_phase = 0.25f;
        const auto rejected_direct_phase = fixture.m_pass.prepare(
            *unit_source, {7u, 11u, 13u, 1u}, invalid_straight, fixture.m_fence.completed());
        PPR_TEST_ASSERT(not rejected_direct_phase and rejected_direct_phase.error() == std::errc::invalid_argument);

        auto unit_generation = fixture.generate(*unit_source, 1u, true);
        const auto unit_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(unit_rows.has_value() and unit_generation->statistics().m_skipped_blocks > 0u);
        for (const auto &row: *unit_rows) {
            PPR_TEST_ASSERT(row.m_status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel)
                PPR_TEST_ASSERT(std::bit_cast<u32>(row.m_coefficients[channel]) == std::bit_cast<u32>(1.0f));
            for (u32 coefficient = 3u; coefficient < 27u; ++coefficient)
                PPR_TEST_ASSERT(row.m_coefficients[coefficient] == 0.0f);
        }

        normalChecks_(fixture, *unit_rows);

        auto straight_unit = fixture.generate(
            *unit_source, 30u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::straight256_reference, 65504.0f, true);
        PPR_TEST_ASSERT(straight_unit->maximumResponse() == 65504.0f);
        const auto straight_unit_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(straight_unit_rows.has_value() and straight_unit_rows->size() == 64u);

        for (const auto &row: *straight_unit_rows) {
            PPR_TEST_ASSERT(row.m_status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel)
                PPR_TEST_ASSERT(std::bit_cast<u32>(row.m_coefficients[channel]) == std::bit_cast<u32>(1.0f));
            for (u32 coefficient = 3u; coefficient < 27u; ++coefficient)
                PPR_TEST_ASSERT(row.m_coefficients[coefficient] == 0.0f);
        }

        normalChecks_(fixture, *straight_unit_rows);

        materialChecks_(fixture);

        worldGridChecks_(fixture);


        const lighting::OpticalCell huge_source{
            {}, {}, {std::ldexp(1.0, 125), 0.0, 0.0}, lighting::OpticalKind::opaque};
        const std::array<u16, 1> huge_ids{17u};
        const std::array<lighting::WorldRadianceMaterial, 2> huge_palette{{{vacuum, 0u}, {huge_source, 17u}}};
        const auto huge = lighting::WorldRadianceSource::capture(
            {huge_ids, huge_palette, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum, {}});
        // Large finite sources outside the source-wide FTZ loss domain reject
        // before allocation, instead of silently erasing visible far radiance.
        PPR_TEST_ASSERT(not huge and huge.error() == std::errc::argument_out_of_domain);

        // FP32 transport still admits this source. The SAME immutable input
        // must fail publication when explicitly requested for half-float use.
        const lighting::OpticalCell half_overflow_source{{}, {}, {100000.0, 0.0, 0.0}, lighting::OpticalKind::opaque};
        const std::array<lighting::WorldRadianceMaterial, 2> half_palette{{{vacuum, 0u}, {half_overflow_source, 17u}}};
        const auto half_source = lighting::WorldRadianceSource::capture(
            {huge_ids, half_palette, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(half_source.has_value());

        const auto float_generation = fixture.generate(
            *half_source, 37u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::straight256_reference);
        PPR_TEST_ASSERT(float_generation->maximumResponse() == (std::numeric_limits<float>::max)());
        const auto float_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(float_rows.has_value() and float_rows->size() == 1u);
        PPR_TEST_ASSERT(float_rows->front().m_coefficients[0] == 100000.0f);

        lighting::WorldRadianceSettings half_settings{
            64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, 0u, .5f, false, false,
            lighting::WorldRadianceRefinedSpacing::native_cell_centres,
            lighting::WorldRadianceTransport::straight256_reference};
        half_settings.m_maximum_response = 65504.0f;
        for (const float invalid_limit: {0.0f, std::numeric_limits<float>::quiet_NaN()}) {
            auto invalid = half_settings;
            invalid.m_maximum_response = invalid_limit;
            const auto rejected_limit = fixture.m_pass.prepare(
                *half_source, {7u, 11u, 13u, 38u}, invalid, fixture.m_fence.completed());
            PPR_TEST_ASSERT(not rejected_limit and rejected_limit.error() == std::errc::invalid_argument);
        }

        const auto half_demand = fixture.m_pass.prepare(
            *half_source, {7u, 11u, 13u, 38u}, half_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(half_demand.has_value() and not half_demand->m_admission_error);
        rhi::ComPtr<rhi::ICommandEncoder> half_encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(half_encoder.writeRef())));
        PPR_TEST_ASSERT(
            not fixture.m_pass.encode({fixture.m_rhi->getDevice(), *half_encoder, fixture.m_fence.completed()}));

        fixture.submit(*half_encoder, true);

        const auto half_overflow = fixture.m_pass.acquire(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not half_overflow and half_overflow.error() == std::errc::result_out_of_range);
        half_encoder.setNull();

        const lighting::OpticalCell safe_half_source{{}, {}, {2.0, 0.0, 0.0}, lighting::OpticalKind::opaque};
        const std::array<lighting::WorldRadianceMaterial, 2> safe_half_palette{{{vacuum, 0u}, {safe_half_source, 17u}}};
        const auto safe_half = lighting::WorldRadianceSource::capture(
            {huge_ids, safe_half_palette, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum, {}});
        PPR_TEST_ASSERT(safe_half.has_value());

        const auto half_generation = fixture.generate(
            *safe_half, 39u, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
            lighting::WorldRadianceTransport::straight256_reference, 65504.0f);
        PPR_TEST_ASSERT(half_generation->maximumResponse() == 65504.0f);
        const auto safe_rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
        PPR_TEST_ASSERT(safe_rows.has_value() and safe_rows->size() == 1u);
        PPR_TEST_ASSERT(safe_rows->front().m_coefficients[0] == 2.0f);

        Shape initial_face;
        initial_face.m_width = 8u;
        initial_face.m_height = 8u;
        initial_face.m_ids.resize(64u);
        initial_face.m_cells.resize(64u, vacuum);
        const lighting::OpticalCell sole_emitter{{}, {}, {2.0, 1.0, 0.5}, lighting::OpticalKind::opaque};
        initial_face.m_materials = {{vacuum, 0u}, {sole_emitter, 23u}};
        initial_face.m_ids[3u * 8u + 5u] = 23u;
        initial_face.m_cells[3u * 8u + 5u] = sole_emitter;
        traceChecks_(fixture, initial_face, true);

        std::println(
            "WORLD_RC_CONSTANT verdict=PASS front_exact=1 signed_ac_zero=1 rotated_K16_constant_error_max=0.01");

        // A larger unsubmitted candidate overlaps a smaller replacement, then
        // retires. Its old reported peak must not become the smaller batch's
        // own persistent allocation ceiling on a subsequent preparation.
        const std::array<u16, 64u * 36u> large_ids{};
        const auto large_source = lighting::WorldRadianceSource::capture(
            {large_ids, vacuum_palette, {}, 1.0, 64u, 36u, lighting::OpticalBoundary::vacuum, {1.0, 1.0, 1.0}});
        PPR_TEST_ASSERT(large_source.has_value());
        lighting::WorldRadianceSettings capacity_settings{
            64u * 1024u * 1024u, 1024u * 1024u, 32u * 1024u * 1024u, 4u, 0.5f, true};
        const auto large = fixture.m_pass.prepare(
            *large_source, {7u, 11u, 13u, 21u}, capacity_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(large.has_value() and not large->m_admission_error);
        const auto small = fixture.m_pass.prepare(
            *unit_source, {7u, 11u, 13u, 22u}, capacity_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(small.has_value() and not small->m_admission_error);
        const u64 small_ceiling = std::max(
            {small->m_parity_bytes[0], small->m_parity_bytes[1], small->m_source_gpu_bytes, small->m_refined_bytes,
                small->m_readback_bytes});
        PPR_TEST_ASSERT(large->m_largest_resource_bytes > small_ceiling);
        PPR_TEST_ASSERT(small->m_largest_resource_bytes > small_ceiling); // The live large overlap is reported.

        capacity_settings.m_max_resource_bytes = small_ceiling;
        const auto reduced_cap = fixture.m_pass.prepare(
            *unit_source, {7u, 11u, 13u, 23u}, capacity_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(reduced_cap.has_value() and not reduced_cap->m_admission_error);
        PPR_TEST_ASSERT(reduced_cap->m_largest_resource_bytes <= small_ceiling);

        struct PhysicalObservation {
            std::array<std::shared_ptr<const lighting::WorldRadianceGeneration>, 2> m_retained{};
            double m_direct{}, m_measured{}, m_absolute_error{}, m_closed{}, m_closed_direct{}, m_open{};
            double m_sampled{}, m_sampling_error{}, m_traversal_error{}, m_traversal_max{};
            bool m_brightness_pass{}, m_leakage_pass{}, m_traversal_pass{true};
        };

        PhysicalObservation straight;
        for (u32 state = 0u; state < 2u; ++state) {
            const bool open = state != 0u;
            const auto shape = shape_(false, open);
            if (not open) {
                auto unsupported = shape.m_materials;
                const auto source_row = std::ranges::find(
                    unsupported, 23u, &lighting::WorldRadianceMaterial::m_material_id);
                PPR_TEST_ASSERT(source_row != unsupported.end());
                source_row->m_optics = {{{1.0, 1.0, 1.0}}, {}, {}, lighting::OpticalKind::unavailable};
                const auto rejected_media = lighting::WorldRadianceSource::capture(
                    {shape.m_ids, unsupported, {}, 1.0, shape.m_width, shape.m_height,
                        lighting::OpticalBoundary::vacuum, {}});
                PPR_TEST_ASSERT(not rejected_media and rejected_media.error() == std::errc::not_supported);
            }

            traceChecks_(fixture, shape);

            straight.m_retained[state] = fixture.generate(
                shape.m_source, 2u + state, false, lighting::WorldRadianceRefinedSpacing::native_cell_centres, 0u,
                lighting::WorldRadianceTransport::straight256_reference);
            const auto rows = fixture.m_pass.readDiagnosticCoefficients(fixture.m_fence.completed());
            PPR_TEST_ASSERT(rows.has_value());

            for (u32 screen_y = 3u; screen_y <= 6u; ++screen_y) {
                for (u32 x = 7u; x <= 9u; ++x) {
                    const std::array<double, 2> point{double(x) + 0.5, double(shape.m_height - screen_y - 1u) + 0.5};
                    const double reference = directMean_(shape, point);
                    const double sampled = directMean_(shape, point, true);
                    const auto &generation = *straight.m_retained[state];
                    PPR_TEST_ASSERT(generation.transport() == lighting::WorldRadianceTransport::straight256_reference);
                    PPR_TEST_ASSERT(point[0] - 0.5 == std::floor(point[0] - 0.5));
                    PPR_TEST_ASSERT(point[1] - 0.5 == std::floor(point[1] - 0.5));
                    const auto value = meanAt_(generation, *rows, point);
                    PPR_TEST_ASSERT(std::isfinite(value[0]) and value[0] >= 0.0);
                    const double displayed = std::max(value[0], 0.0); // Final front-normal physical response only.
                    const double traversal_error = std::abs(value[0] - sampled);
                    straight.m_traversal_error += traversal_error;
                    straight.m_traversal_max = std::max(straight.m_traversal_max, traversal_error);
                    straight.m_traversal_pass = straight.m_traversal_pass and
                                                traversal_error <= 1e-5 + 1e-4 * std::abs(sampled);

                    if (open) {
                        straight.m_direct += reference;
                        straight.m_measured += displayed;
                        straight.m_absolute_error += std::abs(displayed - reference);
                        straight.m_sampled += sampled;
                        straight.m_sampling_error += std::abs(sampled - reference);
                        straight.m_open += displayed;
                    } else {
                        straight.m_closed_direct += reference;
                        straight.m_closed += displayed;
                    }

                    std::println(
                        "WORLD_STRAIGHT256_POINT state={} x={} y={} direct1024={} slab_uploaded256={} raw_gpu_ch_dc={} displayed_front={} sampling_abs_error={} traversal_abs_error={} physical_abs_error={}",
                        open ? "open" : "closed", point[0], point[1], reference, sampled, value[0], displayed,
                        std::abs(sampled - reference), traversal_error, std::abs(displayed - reference));
                }
            }
        }

        straight.m_brightness_pass = straight.m_direct > 0.0 and straight.m_absolute_error <= 0.15 * straight.m_direct;
        straight.m_leakage_pass = straight.m_closed_direct == 0.0 and
                                  straight.m_open > 0.0 and
                                  straight.m_closed <= 0.01 * straight.m_open;
        std::println(
            "WORLD_STRAIGHT256_PHYSICAL small_scene_reference_only=1 scalable_RC_qualified=0 brightness_domain=open_only direct1024_sum={} slab256_sum={} gpu_sum={} sum_abs_errors={} signed_bias={} L1={} sampling_L1={} traversal_abs_sum={} traversal_abs_max={} traversal_abs_tolerance=1e-5 traversal_rel_tolerance=1e-4 traversal_pass={} closed_direct={} closed_gpu={} open_gpu={} L1_gate=0.15 blocked_gate=0.01 brightness_pass={} leakage_pass={}",
            straight.m_direct, straight.m_sampled, straight.m_measured, straight.m_absolute_error,
            straight.m_measured - straight.m_direct,
            straight.m_direct > 0.0 ? straight.m_absolute_error / straight.m_direct : -1.0,
            straight.m_direct > 0.0 ? straight.m_sampling_error / straight.m_direct : -1.0, straight.m_traversal_error,
            straight.m_traversal_max, straight.m_traversal_pass, straight.m_closed_direct, straight.m_closed,
            straight.m_open, straight.m_brightness_pass, straight.m_leakage_pass);
        std::ignore = std::fflush(nullptr); // Preserve sampling/traversal/physical verdicts before any gate.

        // The pilot deliberately retains field generations in Grid roots.
        // Run it after the existing small-capacity retirement assertions so
        // those tests keep their original live-owner/admission conditions.
        screenContactChecks_(fixture);

        // Rejected plane/fixed16/terminal-C1 source and raw evidence remain
        // archived. Active qualification now exercises the production window.
        directWindowChecks_(fixture, straight.m_retained);
        homogeneousChecks_(fixture);

        discoveryChecks_(fixture);
        singleBounceChecks_(fixture);
        overlayChecks_(fixture);

        auto matter_fault = committedMatterChecks_(fixture, fixture.m_matter_pass);

        // Timing remains pass/batch observation, never a mutation of a published
        // generation. Retiring never-submitted timing has an explicit terminal status.
        auto timing_settings = capacity_settings;
        timing_settings.m_max_resource_bytes = 32u * 1024u * 1024u;
        timing_settings.m_measure_gpu = true;
        const lighting::TerrainReceiverCohort retired_cohort{7u, 11u, 13u, 100u};
        const lighting::TerrainReceiverCohort canceled_cohort{7u, 11u, 13u, 101u};
        const lighting::TerrainReceiverCohort unknown_cohort{7u, 11u, 13u, 102u};
        const auto retire_candidate = fixture.m_pass.prepare(
            *unit_source, retired_cohort, timing_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(retire_candidate and not retire_candidate->m_admission_error);

        const auto canceled_candidate = fixture.m_pass.prepare(
            *unit_source, canceled_cohort, timing_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(canceled_candidate and not canceled_candidate->m_admission_error);
        const auto retired_timing = fixture.m_pass.pollTiming(retired_cohort, fixture.m_fence.completed());
        if (fixture.m_rhi->getDevice().getInfo().timestampFrequency != 0u) {
            PPR_TEST_ASSERT(retired_timing.m_status == lighting::WorldRadianceTimingStatus::retired_unresolved);
        }

        PPR_TEST_ASSERT(not retired_timing.m_milliseconds and retired_timing.m_submission == kFenceValueNone);
        rhi::ComPtr<rhi::ICommandEncoder> canceled_encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(canceled_encoder.writeRef())));
        PPR_TEST_ASSERT(
            not fixture.m_pass.encode({fixture.m_rhi->getDevice(), *canceled_encoder, fixture.m_fence.completed()}));
        fixture.submit(*canceled_encoder, true);
        const auto canceled_submission = fixture.m_fence.lastSubmitted();
        const auto canceled_before_completion = fixture.m_pass.pollTiming(canceled_cohort, kFenceValueNone);
        PPR_TEST_ASSERT(not canceled_before_completion.m_milliseconds);

        // Match the completed numeric-timing assertion above: the canceled
        // submission's app fence alone does not prove D3D12 host query readiness.
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

        // A canceled accepted request is deliberately never acquired/published.
        // Completion permits replacement, while its timing keeps the real fence.
        const auto unknown_candidate = fixture.m_pass.prepare(
            *unit_source, unknown_cohort, timing_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(unknown_candidate and not unknown_candidate->m_admission_error);
        const auto canceled_timing = fixture.m_pass.pollTiming(canceled_cohort, fixture.m_fence.completed());
        PPR_TEST_ASSERT(canceled_timing.m_submission == canceled_submission);
        if (fixture.m_rhi->getDevice().getInfo().timestampFrequency != 0u) {
            PPR_TEST_ASSERT(canceled_timing.m_status == lighting::WorldRadianceTimingStatus::ready);
        }

        const auto matter_canceled = fixture.m_matter_pass.cancelMatter(fixture.m_fence.completed());
        PPR_TEST_ASSERT(matter_canceled.has_value() and *matter_canceled);

        const lighting::WorldRadianceMatterSettings unknown_matter_settings{
            64ull * 1024u * 1024u, 1024ull * 1024u, 32ull * 1024u * 1024u, 1400u, false};
        const auto unknown_matter = fixture.m_matter_pass.prepareMatter(
            matter_fault.first, matter_fault.second, unknown_matter_settings, fixture.m_fence.completed());
        PPR_TEST_ASSERT(unknown_matter.has_value() and not unknown_matter->m_admission_error);

        const std::weak_ptr<const GridParticleMetadataResource> unknown_matter_metadata =
            matter_fault.second.m_metadata;
        rhi::ComPtr<rhi::ICommandEncoder> unknown_matter_encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(unknown_matter_encoder.writeRef())));
        PPR_TEST_ASSERT(not fixture.m_matter_pass.encodeMatter(
            {fixture.m_rhi->getDevice(), *unknown_matter_encoder, fixture.m_fence.completed()}, 1401u));

        matter_fault.second = {};

        rhi::ComPtr<rhi::ICommandEncoder> unknown_encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(unknown_encoder.writeRef())));
        PPR_TEST_ASSERT(
            not fixture.m_pass.encode({fixture.m_rhi->getDevice(), *unknown_encoder, fixture.m_fence.completed()}));

        PPR_TEST_ASSERT(not fixture.m_matter_pass.notifySubmitFailed());
        PPR_TEST_ASSERT(not fixture.m_matter_pass.canEncodeMatter());
        const auto unavailable_matter = fixture.m_matter_pass.acquireMatter(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not unavailable_matter and unavailable_matter.error() == std::errc::state_not_recoverable);

        const auto quarantined_matter = fixture.m_matter_pass.cancelMatter(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not quarantined_matter and quarantined_matter.error() == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not unknown_matter_metadata.expired());

        PPR_TEST_ASSERT(not fixture.m_pass.notifySubmitFailed());
        PPR_TEST_ASSERT(fixture.m_pass.prepareDiscoveryPipeline() == std::errc::operation_not_permitted);

        const auto unknown_timing = fixture.m_pass.pollTiming(unknown_cohort, fixture.m_fence.completed());
        if (fixture.m_rhi->getDevice().getInfo().timestampFrequency != 0u) {
            PPR_TEST_ASSERT(unknown_timing.m_status == lighting::WorldRadianceTimingStatus::failed);
            PPR_TEST_ASSERT(unknown_timing.m_error == std::errc::state_not_recoverable);
        }

        const auto unknown_error = std::make_error_code(std::errc::io_error);
        PPR_TEST_ASSERT(fixture.m_matter_pass.shutdown({GpuQuiescence::kUnknown, unknown_error}) == unknown_error);
        PPR_TEST_ASSERT(not unknown_matter_metadata.expired());

        PPR_TEST_ASSERT(fixture.m_pass.shutdown({GpuQuiescence::kUnknown, unknown_error}) == unknown_error);
        const auto still_retained = fixture.m_pass.pollTiming(unknown_cohort, fixture.m_fence.completed());
        PPR_TEST_ASSERT(still_retained.m_status == unknown_timing.m_status);
        PPR_TEST_ASSERT(still_retained.m_cohort == unknown_cohort and not still_retained.m_milliseconds);

        // Both failure notifications intentionally modeled unknown fate without
        // executing submissions. Dropping both encoders proves they cannot become
        // submitted; all earlier real submissions were explicitly waited.
        unknown_encoder.setNull();
        unknown_matter_encoder.setNull();

        // Both straight-field source generations and the two constant controls
        // survive producer shutdown. Their leases remain owned before RHI teardown.
        PPR_TEST_ASSERT(not fixture.m_matter_pass.shutdown({GpuQuiescence::kIdleProven, {}}));
        PPR_TEST_ASSERT(unknown_matter_metadata.expired());

        PPR_TEST_ASSERT(not fixture.m_pass.shutdown({GpuQuiescence::kIdleProven, {}}));
        const auto after_shutdown = fixture.read<lighting::WorldRadianceCoefficients>(
            unit_generation->coefficientsBuffer());
        PPR_TEST_ASSERT(after_shutdown.size() == unit_rows->size() and after_shutdown[0].m_coefficients[0] == 1.0f);
        PPR_TEST_ASSERT(
            straight.m_retained[0]->cohort().m_geometry_revision == 2u and
            straight.m_retained[1]->cohort().m_geometry_revision == 3u);

        const auto straight_after_shutdown = fixture.read<lighting::WorldRadianceCoefficients>(
            straight_unit->coefficientsBuffer());
        PPR_TEST_ASSERT(straight_after_shutdown.size() == 64u and straight_after_shutdown[0].m_coefficients[0] == 1.0f);

        unit_generation.reset();
        straight_unit.reset();
        straight.m_retained = {};

        PPR_TEST_ASSERT(straight.m_traversal_pass);
        PPR_TEST_ASSERT(straight.m_brightness_pass);
        PPR_TEST_ASSERT(straight.m_leakage_pass);
    };
}

namespace pP::tests {
    const UnitTest world_radiance_gpu = UnitTest::Named("world_radiance_gpu") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::WorldRadianceGpu::temporal_world_profile_admission,
                detail::WorldRadianceGpu::temporal_world_field_sequence,
                detail::WorldRadianceGpu::
                    native_cell_candidate_shared_analytic_wall_aperture_and_constant_normal_contracts});
    };

    const UnitTest &world_radiance_gpuTests() noexcept {
        return world_radiance_gpu;
    }
}
