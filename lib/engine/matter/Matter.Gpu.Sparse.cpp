module;
#include "pP/Macros.h"
#include <slang.h>

module engine.matter;

import :gpu.sparse;
import :gpu;
import std;

namespace pP::matter {
    // ------------------------------------------------------------------
    // checked admission bounds and buffer contracts
    // ------------------------------------------------------------------

    static_assert(sizeof(SparseIndex) == 8u and alignof(SparseIndex) == 4u);
    static_assert(sizeof(SparseBucket) == 16u and alignof(SparseBucket) == 4u);
    static_assert(PPR_OFFSETOF(SparseBucket, m_row_start) == 8u);
    static_assert(sizeof(SparseGroup) == 16u and alignof(SparseGroup) == 4u);
    static_assert(sizeof(SparseConstants) == 64u and alignof(SparseConstants) == 4u);
    static_assert(PPR_OFFSETOF(SparseConstants, m_generation) == 32u);
    static_assert(PPR_OFFSETOF(SparseConstants, m_bucket_keys) == 48u);

    Expected<SparseCapacity> sparseCapacity(
        const u32 rows, const u32 width, const u32 height, const rhi::DeviceLimits &limits) {
        const u64 nodes = static_cast<u64>(width) * height;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterGpu, width >= 9u and
                               height >= 9u and
                               nodes <= std::numeric_limits<u32>::max() / 4u and
                               rows <= std::numeric_limits<u32>::max() - 63u and
                               limits.maxComputeThreadsPerGroup >= 64u and
                               limits.maxComputeThreadGroupSize[0] >= 64u and
                               limits.maxComputeDispatchThreadGroups[0] != 0u and
                               limits.maxComputeDispatchThreadGroups[1] != 0u
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        const u64 bucket_columns = (static_cast<u64>(width) + 5u) / 6u;
        const u64 page_columns = (static_cast<u64>(width) + 7u) / 8u;
        const u64 bucket_keys = bucket_columns * ((static_cast<u64>(height) + 5u) / 6u);
        const u64 page_keys = page_columns * ((static_cast<u64>(height) + 7u) / 8u);
        const u64 buckets = std::min<u64>(rows, bucket_keys);
        const u64 groups = std::min<u64>(rows, (static_cast<u64>(rows) + 63u * buckets) / 64u);
        const u64 pages = std::min(6u * buckets, page_keys);
        const u64 words = pages * 64u * 4u;
        const u64 dispatch_capacity = static_cast<u64>(limits.maxComputeDispatchThreadGroups[0]) *
                                      limits.maxComputeDispatchThreadGroups[1];
        const u64 maximum_groups = std::max({buckets, pages, groups, (static_cast<u64>(rows) + 63u) / 64u});
        const std::array<u64, 8u> sizes{
            bucket_keys * 8u, page_keys * 8u, std::max<u64>(1u, buckets) * 16u, std::max<u64>(1u, pages) * 4u,
            std::max<u64>(1u, words) * 4u, std::max<u64>(1u, groups) * 16u, std::max<u64>(1u, rows) * 4u, 48u};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterGpu,
            words <= std::numeric_limits<u32>::max() and
                    maximum_groups <= dispatch_capacity and
                    std::ranges::all_of(sizes, [&](const u64 bytes) { return bytes <= limits.maxBufferSize; })
                ? std::error_code{}
                : std::make_error_code(std::errc::value_too_large));

        SparseCapacity result{};
        result.m_lookup_bytes = sizes[0u] + 2u * sizes[1u];
        result.m_dynamic_bytes = sizes[2u] + sizes[3u] + 3u * sizes[4u] + sizes[5u] + sizes[6u] + 16u + 48u + 24u;
        result.m_width = width;
        result.m_height = height;
        result.m_admitted_row_capacity = rows;
        result.m_bucket_keys = static_cast<u32>(bucket_keys);
        result.m_page_keys = static_cast<u32>(page_keys);
        result.m_bucket_columns = static_cast<u32>(bucket_columns);
        result.m_page_columns = static_cast<u32>(page_columns);
        result.m_buckets = static_cast<u32>(buckets);
        result.m_pages = static_cast<u32>(pages);
        result.m_groups = static_cast<u32>(groups);
        result.m_dispatch_width = limits.maxComputeDispatchThreadGroups[0];
        result.m_dispatch_height = limits.maxComputeDispatchThreadGroups[1];
        return result;
    }

    namespace {
        constexpr std::array<const char *, 7u> kSparseEntries{
            "discoverBuckets", "finalizeSparseArguments", "initializeBucketsAndDiscoverPages", "countSparseParticles",
            "allocateSparseGroups", "insertSparseParticles", "initializeSparseDestination"};

        struct SparseKernel {
            rhi::ComPtr<rhi::IShaderProgram> m_program;
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline;
        };

        rhi::BufferRange sparseRange(rhi::IBuffer *const buffer) {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        std::error_code sparseBuffer(
            rhi::IDevice &device, const u64 count, const u32 stride, const char *const label,
            rhi::ComPtr<rhi::IBuffer> &output, const bool indirect = false) {
            rhi::BufferDesc desc{};
            desc.size = std::max<u64>(1u, count) * stride;
            desc.elementSize = stride;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource) |
                static_cast<u32>(rhi::BufferUsage::CopyDestination));
            if (indirect)
                desc.usage = static_cast<rhi::BufferUsage>(
                    static_cast<u32>(desc.usage) | static_cast<u32>(rhi::BufferUsage::IndirectArgument));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            desc.label = label;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, device.createBuffer(desc, nullptr, output.writeRef()));
            return {};
        }
    }

    // ------------------------------------------------------------------
    // immutable maintenance encoding
    // ------------------------------------------------------------------

    struct SparseGridState {
        std::array<SparseKernel, kSparseEntries.size()> m_kernels;
        SparseBuffers m_buffers;
        SparseCapacity m_capacity;
        rhi::IDevice *m_device{};
        rhi::IBuffer *m_hot{}, *m_cold{};
        GpuParameters m_profile{};
        u32 m_next_generation{1u}, m_generation{}, m_source_generation{}, m_remaining_substeps{};
        u32 m_buckets{}, m_pages{}, m_groups{};
        bool m_dirty_indexes{true};
        bool m_encoding_open{};

        SparseConstants constants(const bool destination_a) const noexcept {
            return {
                m_capacity.m_width, m_capacity.m_height, m_profile.m_particle_count, m_capacity.m_bucket_columns,
                m_capacity.m_page_columns, m_buckets, m_pages, m_groups, m_generation, m_source_generation,
                m_capacity.m_dispatch_width, destination_a ? 1u : 0u, m_capacity.m_bucket_keys, m_capacity.m_page_keys};
        }

        std::error_code encode(
            rhi::ICommandEncoder &encoder, const u32 kernel, const u32 direct_groups, const i32 indirect,
            const bool destination_a, Array<rhi::ComPtr<rhi::IShaderObject>> &retained_roots) {
            const auto &program = m_kernels[kernel];
            rhi::ComPtr<rhi::IShaderObject> root;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, m_device->createRootShaderObject(program.m_program, root.writeRef()));
            const rhi::ShaderCursor cursor{root};
            rhi::ShaderCursor constant_cursor;
            const auto sparse = constants(destination_a);
            const auto set = [&](const char *const name, const void *const data, const u64 bytes) -> std::error_code {
                if (not cursor[name].isValid())
                    return {};
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, cursor[name].getDereferenced(constant_cursor));
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, constant_cursor.setData(data, bytes));
                return {};
            };
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, set("g_parameters", &m_profile, sizeof(m_profile)));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, set("g_sparse", &sparse, sizeof(sparse)));

            const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                if (not cursor[name].isValid())
                    return {};
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, cursor[name].setBinding(rhi::Binding(buffer, sparseRange(buffer))));
                return {};
            };
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_hot", m_hot));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_cold", m_cold));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_bucketIndex", m_buffers.m_bucket_index));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_activeBuckets", m_buffers.m_active_buckets));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_pageIndexA", m_buffers.m_page_index[0u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_pageIndexB", m_buffers.m_page_index[1u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_pageKeys", m_buffers.m_page_keys));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_payloadA", m_buffers.m_payload[0u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_payloadB", m_buffers.m_payload[1u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_sparseBudget", m_buffers.m_budget));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_sparseCounters", m_buffers.m_counters));
            // Only the finalizer writes arguments. Binding this UAV to an
            // indirect consumer would request an unrelated UAV transition.
            if (kernel == 1u)
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_sparseArgs", m_buffers.m_arguments));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_buckets", m_buffers.m_groups));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_rows", m_buffers.m_rows));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_errors", m_buffers.m_errors));
            retained_roots.push_back(root);

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, pass ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(program.m_pipeline, root);
                if (indirect >= 0)
                    pass->dispatchComputeIndirect({m_buffers.m_arguments.get(), static_cast<u64>(indirect) * 16u});
                else {
                    const u32 width = std::min(direct_groups, m_capacity.m_dispatch_width);
                    const u32 height = direct_groups == 0u ? 1u
                                                           : (direct_groups - 1u) / m_capacity.m_dispatch_width + 1u;
                    pass->dispatchCompute(width, height, 1u);
                }
            }

            encoder.globalBarrier();
            return {};
        }
    };

    SparseGridStorage::SparseGridStorage() = default;
    SparseGridStorage::~SparseGridStorage() = default;
    SparseGridStorage::SparseGridStorage(SparseGridStorage &&) noexcept = default;

    SparseGridStorage &SparseGridStorage::operator=(SparseGridStorage &&) noexcept = default;

    std::error_code SparseGridStorage::initialize(
        rhi::IDevice &device, IShaderService &shaders, const std::filesystem::path &root, const u32 admitted_rows,
        const u32 width, const u32 height) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, not m_state ? std::error_code{} : std::make_error_code(std::errc::already_connected));
        const auto capacity = sparseCapacity(admitted_rows, width, height, device.getInfo().limits);
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, capacity);

        auto state = std::make_unique<SparseGridState>();
        state->m_capacity = *capacity;
        state->m_device = &device;
        state->m_buckets = capacity->m_buckets;
        state->m_pages = capacity->m_pages;
        state->m_groups = capacity->m_groups;
        auto &buffers = state->m_buffers;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, sparseBuffer(device, capacity->m_bucket_keys, 8u, "PBMPM bucket index", buffers.m_bucket_index));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            sparseBuffer(device, capacity->m_buckets, 16u, "PBMPM active buckets", buffers.m_active_buckets));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, sparseBuffer(device, capacity->m_pages, 4u, "PBMPM current page keys", buffers.m_page_keys));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, sparseBuffer(
                           device, static_cast<u64>(capacity->m_pages) * 256u, 4u, "PBMPM shared sparse budget",
                           buffers.m_budget));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, sparseBuffer(device, capacity->m_groups, 16u, "PBMPM sparse groups", buffers.m_groups));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, sparseBuffer(device, admitted_rows, 4u, "PBMPM sparse row permutation", buffers.m_rows));
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, sparseBuffer(device, 4u, 4u, "PBMPM sparse counters", buffers.m_counters));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, sparseBuffer(device, 12u, 4u, "PBMPM sparse arguments", buffers.m_arguments, true));
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, sparseBuffer(device, 6u, 4u, "PBMPM sparse first error", buffers.m_errors));
        for (u32 index = 0u; index < 2u; ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu,
                sparseBuffer(device, capacity->m_page_keys, 8u, "PBMPM page index", buffers.m_page_index[index]));
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, sparseBuffer(
                               device, static_cast<u64>(capacity->m_pages) * 256u, 4u, "PBMPM sparse payload",
                               buffers.m_payload[index]));
        }

        shader::SharedModule module;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, shaders.loadModuleFromFile(
                           root / "shaders/pbmpm/sparse_grid.slang", "pbmpm_sparse_grid", module.writeRef()));
        for (u32 index = 0u; index < kSparseEntries.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, module->findEntryPointByName(kSparseEntries[index], entry.writeRef()));
            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module.get();
            desc.slangEntryPoints = entries;
            desc.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            auto &kernel = state->m_kernels[index];
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, device.createShaderProgram(desc, kernel.m_program.writeRef(), diagnostics.writeRef()));
            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = kernel.m_program;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, device.createComputePipeline(pipeline, kernel.m_pipeline.writeRef()));
        }

        m_state = std::move(state);
        return {};
    }

    bool SparseGridStorage::canAdmit(const u32 rows) const noexcept {
        return m_state and rows <= m_state->m_capacity.m_admitted_row_capacity;
    }

    const SparseCapacity &SparseGridStorage::capacity() const noexcept {
        PPR_ASSERT(m_state != nullptr);
        return m_state->m_capacity;
    }

    const SparseBuffers &SparseGridStorage::buffers() const noexcept {
        PPR_ASSERT(m_state != nullptr);
        return m_state->m_buffers;
    }

    SparseConstants SparseGridStorage::constants(const bool a) const noexcept {
        PPR_ASSERT(m_state != nullptr);
        return m_state->constants(a);
    }

    std::error_code SparseGridStorage::beginTick(rhi::ICommandEncoder &encoder, const u32 substeps) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            m_state and substeps != 0u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        auto &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            not state.m_encoding_open ? std::error_code{} : std::make_error_code(std::errc::operation_in_progress));

        state.m_encoding_open = true;
        auto &buffers = state.m_buffers;
        encoder.pushDebugGroup("PBMPM.Sparse.TickReset", {});
        PPR_DEFER {
            encoder.popDebugGroup();
        };
        for (auto *buffer:
            {buffers.m_bucket_index.get(), buffers.m_page_index[0u].get(), buffers.m_page_index[1u].get(),
                buffers.m_active_buckets.get(), buffers.m_page_keys.get(), buffers.m_payload[0u].get(),
                buffers.m_payload[1u].get(), buffers.m_budget.get(), buffers.m_groups.get(), buffers.m_rows.get(),
                buffers.m_counters.get(), buffers.m_arguments.get(), buffers.m_errors.get()})
            encoder.setBufferState(buffer, rhi::ResourceState::UnorderedAccess);

        if (state.m_dirty_indexes or
            state.m_next_generation == 0u or
            substeps - 1u > std::numeric_limits<u32>::max() - state.m_next_generation) {
            encoder.clearBuffer(buffers.m_bucket_index, sparseRange(buffers.m_bucket_index));
            for (auto &index: buffers.m_page_index)
                encoder.clearBuffer(index, sparseRange(index));
            state.m_next_generation = 1u;
        }
        state.m_source_generation = 0u;
        state.m_generation = 0u;
        state.m_remaining_substeps = substeps;
        encoder.clearBuffer(buffers.m_errors, sparseRange(buffers.m_errors));
        encoder.globalBarrier();
        return {};
    }

    std::error_code SparseGridStorage::settleEncoding(const SparseEncodingDisposition disposition) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, m_state and m_state->m_encoding_open ? std::error_code{}
                                                            : std::make_error_code(std::errc::operation_not_permitted));

        auto &state = *m_state;
        state.m_dirty_indexes = disposition == SparseEncodingDisposition::not_submitted;
        state.m_encoding_open = false;
        state.m_remaining_substeps = 0u;
        state.m_generation = 0u;
        state.m_source_generation = 0u;
        return {};
    }

    std::error_code SparseGridStorage::discover(
        rhi::ICommandEncoder &encoder, rhi::IBuffer &hot, rhi::IBuffer &cold, const GpuParameters profile,
        Array<rhi::ComPtr<rhi::IShaderObject>> &roots) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            canAdmit(profile.m_particle_count) and
                    m_state->m_remaining_substeps != 0u and
                    profile.m_grid_width == m_state->m_capacity.m_width and
                    profile.m_grid_height == m_state->m_capacity.m_height and
                    hot.getDesc().elementSize == sizeof(ParticleWrite) and
                    cold.getDesc().elementSize == sizeof(ParticleReadonly) and
                    hot.getDesc().size >= static_cast<u64>(profile.m_particle_count) * sizeof(ParticleWrite) and
                    cold.getDesc().size >= static_cast<u64>(profile.m_particle_count) * sizeof(ParticleReadonly)
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));

        auto &state = *m_state;
        state.m_hot = &hot;
        state.m_cold = &cold;
        state.m_profile = profile;
        state.m_generation = state.m_next_generation++;
        --state.m_remaining_substeps;

        encoder.setBufferState(&hot, rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(&cold, rhi::ResourceState::ShaderResource);
        encoder.clearBuffer(state.m_buffers.m_counters, sparseRange(state.m_buffers.m_counters));
        encoder.clearBuffer(state.m_buffers.m_arguments, sparseRange(state.m_buffers.m_arguments));
        encoder.globalBarrier();

        const u32 row_groups = (profile.m_particle_count + 63u) / 64u;
        for (const auto [kernel, indirect]: std::array<std::pair<u32, i32>, 9u>{
                 {{0u, -1}, {1u, -1}, {2u, 0}, {1u, -1}, {3u, -1}, {4u, 0}, {1u, -1}, {5u, -1}, {1u, -1}}}) {
            encoder.pushDebugGroup(kSparseEntries[kernel], {});
            PPR_DEFER {
                encoder.popDebugGroup();
            };
            const u32 direct = kernel == 1u ? 1u : row_groups;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state.encode(encoder, kernel, direct, indirect, false, roots));
        }
        return {};
    }

    std::error_code SparseGridStorage::initializeDestination(
        rhi::ICommandEncoder &encoder, const bool a, Array<rhi::ComPtr<rhi::IShaderObject>> &roots) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, m_state and m_state->m_generation != 0u
                           ? std::error_code{}
                           : std::make_error_code(std::errc::operation_not_permitted));
        auto &state = *m_state;
        encoder.pushDebugGroup(
            a ? "PBMPM.Sparse.RemapA.PayloadAndBudgetZero" : "PBMPM.Sparse.B.PayloadAndBudgetZero", {});
        PPR_DEFER {
            encoder.popDebugGroup();
        };
        encoder.globalBarrier();
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state.encode(encoder, 6u, 0u, 1, a, roots));
        if (a)
            state.m_source_generation = state.m_generation;
        return {};
    }

    void SparseGridStorage::forceNextGenerationForTest(const u32 generation) noexcept {
        PPR_ASSERT(m_state != nullptr);
        m_state->m_next_generation = generation;
    }

    void SparseGridStorage::reduceCapacitiesForTest(const u32 buckets, const u32 pages, const u32 groups) noexcept {
        PPR_ASSERT(
            m_state and
            buckets <= m_state->m_capacity.m_buckets and
            pages <= m_state->m_capacity.m_pages and
            groups <= m_state->m_capacity.m_groups);
        m_state->m_buckets = buckets;
        m_state->m_pages = pages;
        m_state->m_groups = groups;
    }
}
