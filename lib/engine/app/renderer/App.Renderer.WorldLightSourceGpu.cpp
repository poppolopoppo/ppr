module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;

import :renderer.world_light_source_gpu;
import :renderer.world_light_source;
import :renderer.world_light_types;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

namespace pP::lighting {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(WorldLightSourceGpu, info, none)

    // ------------------------------------------------------------------
    // dense world light source GPU mirror
    // ------------------------------------------------------------------

    namespace {
        inline constexpr u32 kChunkEdge = 128u;
        inline constexpr u64 kChunkCellCount = u64{kChunkEdge} * kChunkEdge;
        inline constexpr u32 kOccupancyBlockEdge = 8u;
        inline constexpr u32 kNumthreads = 64u;
        inline constexpr u32 kOverlayStride = 7u;

        static_assert(sizeof(WorldLightMatterParticle) == 12u and alignof(WorldLightMatterParticle) == 4u);
        static_assert(
            std::is_standard_layout_v<WorldLightMatterParticle> and
            std::is_trivially_copyable_v<WorldLightMatterParticle>);
        static_assert(PPR_OFFSETOF(WorldLightMatterParticle, m_fill) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightMatterParticle, m_material) == 8u);

        static_assert(sizeof(WorldLightMaterial) == 52u and alignof(WorldLightMaterial) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightMaterial, m_sigma) == 16u);
        static_assert(PPR_OFFSETOF(WorldLightMaterial, m_emission) == 28u);
        static_assert(PPR_OFFSETOF(WorldLightMaterial, m_albedo) == 40u);

        static_assert(sizeof(WorldLightSourceConstants) == 32u and alignof(WorldLightSourceConstants) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightSourceConstants, m_has_overlay) == 20u);

        // Per-chunk upload dispatch constants, matching the Slang layout.
        struct GpuChunkUpload {
            u32 m_chunk_x{}, m_chunk_y{};
            u32 m_pad0{}, m_pad1{};
        };
        static_assert(sizeof(GpuChunkUpload) == 16u and alignof(GpuChunkUpload) == 4u);

        std::error_code require_(const bool valid, const std::errc error = std::errc::invalid_argument) noexcept {
            return valid ? std::error_code{} : std::make_error_code(error);
        }

        rhi::BufferRange range_(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }
    }

    struct WorldLightSourceGpu::State {
        // CPU mirror: dense ids and the committed edit state.
        WorldLightPlan m_plan{};
        u64 m_revision{};
        std::vector<WorldLightMaterial> m_materials{};
        std::vector<u16> m_cell_ids{};
        WorldLightDirtyRect m_dirty{};
        std::array<float, 3> m_sky{};
        i32 m_sky_hour{-1};
        u32 m_blocks_x{}, m_blocks_y{}, m_words_per_row{};

        // Borrowed device/shader owners; initialize owner outlives this object.
        rhi::IDevice *m_device{};
        IShaderService *m_shaders{};
        std::filesystem::path m_content_dir;

        // Shader modules, programs, pipelines and bound roots.
        shader::SharedModule m_source_module, m_matter_module;
        rhi::ComPtr<rhi::IShaderProgram> m_upload_program, m_clear_program, m_scatter_program, m_finalize_program,
            m_occupancy_program;
        rhi::ComPtr<rhi::IComputePipeline> m_upload_pipeline, m_clear_pipeline, m_scatter_pipeline, m_finalize_pipeline,
            m_occupancy_pipeline;
        rhi::ComPtr<rhi::IShaderObject> m_upload_root, m_clear_root, m_scatter_root, m_finalize_root, m_occupancy_root;

        // Device buffers.
        rhi::ComPtr<rhi::IBuffer> m_ids_gpu; // DeviceLocal, one u32 per cell.
        rhi::ComPtr<rhi::IBuffer> m_overlay_gpu; // DeviceLocal, 7 u32 per cell.
        rhi::ComPtr<rhi::IBuffer> m_occupancy_gpu; // DeviceLocal, occupancy u32 words.
        rhi::ComPtr<rhi::IBuffer> m_materials_gpu; // DeviceLocal, palette WorldLightMaterial rows.
        rhi::ComPtr<rhi::IBuffer> m_particles_gpu; // Upload, staged WorldLightMatterParticle records.
        rhi::ComPtr<rhi::IBuffer> m_chunk_staging; // Upload, one chunk's widened u32 ids.

        WorldLightMatterParticle *m_particles_mapped{};
        u32 *m_chunk_staging_mapped{};
        bool m_mapped{};

        WorldLightSourceConstants m_constants{};
        GpuChunkUpload m_upload_constants{};

        // Staged work for the next encode.
        std::vector<u32> m_pending_chunks{};
        std::vector<WorldLightMatterParticle> m_pending_particles{};
        bool m_pending{};
        bool m_ready{};
    };

    WorldLightSourceGpu::WorldLightSourceGpu() : m_state{std::make_unique<State>()} {
    }

    WorldLightSourceGpu::~WorldLightSourceGpu() {
        std::ignore = shutdown();
    }

    std::error_code WorldLightSourceGpu::initialize(
        IRhiService &rhi, IShaderService &shaders, const WorldLightPlan &plan,
        const std::span<const WorldLightMaterial> materials, const std::filesystem::path &content_dir) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(not state.m_ready, std::errc::already_connected));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(plan.m_width != 0u and plan.m_height != 0u));

        state.m_device = &rhi.getDevice();
        state.m_shaders = &shaders;
        state.m_content_dir = content_dir;

        // CPU mirror allocation.
        const u64 cell_count = u64{plan.m_width} * plan.m_height;
        state.m_plan = plan;
        state.m_materials.assign(materials.begin(), materials.end());
        state.m_cell_ids.assign(cell_count, 0u);

        state.m_blocks_x = (plan.m_width + kOccupancyBlockEdge - 1u) / kOccupancyBlockEdge;
        state.m_blocks_y = (plan.m_height + kOccupancyBlockEdge - 1u) / kOccupancyBlockEdge;
        state.m_words_per_row = (state.m_blocks_x + 31u) / 32u;

        state.m_constants.m_width = plan.m_width;
        state.m_constants.m_height = plan.m_height;
        state.m_constants.m_chunk_edge = kChunkEdge;
        state.m_constants.m_occupancy_words = state.m_words_per_row;

        // Load the two shader modules.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, shaders.loadModuleFromFile(
                                     content_dir / TEXT("shaders") / TEXT("world_light_source.slang"),
                                     "world_light_source", state.m_source_module.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, shaders.loadModuleFromFile(
                                     content_dir / TEXT("shaders") / TEXT("world_light_matter.slang"),
                                     "world_light_matter", state.m_matter_module.writeRef()));

        // Build the five compute programs and pipelines.
        const auto build_program = [&](shader::IModule *const module, const char *const name,
                                       rhi::ComPtr<rhi::IShaderProgram> &program,
                                       rhi::ComPtr<rhi::IComputePipeline> &pipeline) -> std::error_code {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, module->findEntryPointByName(name, entry.writeRef()));

            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module;
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;

            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu,
                state.m_device->createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline_desc{};
            pipeline_desc.program = program;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, state.m_device->createComputePipeline(pipeline_desc, pipeline.writeRef()));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            build_program(
                state.m_source_module.get(), "uploadWorldLightChunk", state.m_upload_program, state.m_upload_pipeline));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, build_program(
                                     state.m_source_module.get(), "buildWorldLightOccupancy", state.m_occupancy_program,
                                     state.m_occupancy_pipeline));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            build_program(
                state.m_matter_module.get(), "clearWorldLightMatter", state.m_clear_program, state.m_clear_pipeline));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, build_program(
                                     state.m_matter_module.get(), "scatterWorldLightMatter", state.m_scatter_program,
                                     state.m_scatter_pipeline));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, build_program(
                                     state.m_matter_module.get(), "finishWorldLightMatter", state.m_finalize_program,
                                     state.m_finalize_pipeline));

        // Device buffers (ids and overlay start zeroed so the occupancy pass never
        // reads unwritten cells; the first matter scatter establishes the overlay).
        const u64 ids_bytes = cell_count * sizeof(u32);
        const u64 overlay_bytes = cell_count * kOverlayStride * sizeof(u32);
        const u64 occupancy_bytes = u64{state.m_blocks_y} * state.m_words_per_row * sizeof(u32);
        const u64 particles_bytes = cell_count * sizeof(WorldLightMatterParticle);
        const u64 chunk_staging_bytes = kChunkCellCount * sizeof(u32);
        const u64 materials_bytes = u64{std::max<u64>(1u, state.m_materials.size())} * sizeof(WorldLightMaterial);

        const std::vector<u32> zero_ids(cell_count, 0u);
        const std::vector<u32> zero_overlay(cell_count * kOverlayStride, 0u);

        const auto device_buffer = [&](const u64 bytes, const u32 stride, const void *const initial,
                                       const char *const label, rhi::ComPtr<rhi::IBuffer> &out) -> std::error_code {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = stride;
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            description.defaultState = rhi::ResourceState::UnorderedAccess;
            description.label = label;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, state.m_device->createBuffer(description, initial, out.writeRef()));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            device_buffer(ids_bytes, sizeof(u32), zero_ids.data(), "world light cell ids", state.m_ids_gpu));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            device_buffer(overlay_bytes, sizeof(u32), zero_overlay.data(), "world light overlay", state.m_overlay_gpu));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            device_buffer(occupancy_bytes, sizeof(u32), nullptr, "world light occupancy", state.m_occupancy_gpu));

        {
            // Immutable palette: ShaderResource only, uploaded once.
            rhi::BufferDesc description{};
            description.size = materials_bytes;
            description.elementSize = sizeof(WorldLightMaterial);
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = rhi::BufferUsage::ShaderResource;
            description.defaultState = rhi::ResourceState::ShaderResource;
            description.label = "world light materials";
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu,
                state.m_device->createBuffer(description, state.m_materials.data(), state.m_materials_gpu.writeRef()));
        }

        const auto upload_buffer = [&](const u64 bytes, const u32 stride, const char *const label,
                                       rhi::ComPtr<rhi::IBuffer> &out) -> std::error_code {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = stride;
            description.memoryType = rhi::MemoryType::Upload;
            description.usage = rhi::BufferUsage::ShaderResource;
            description.defaultState = rhi::ResourceState::ShaderResource;
            description.label = label;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, state.m_device->createBuffer(description, nullptr, out.writeRef()));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, upload_buffer(
                                     particles_bytes, sizeof(WorldLightMatterParticle), "world light matter particles",
                                     state.m_particles_gpu));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            upload_buffer(chunk_staging_bytes, sizeof(u32), "world light chunk staging", state.m_chunk_staging));

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, make_error_code(state.m_device->mapBuffer(
                                     state.m_particles_gpu.get(), rhi::CpuAccessMode::Write,
                                     reinterpret_cast<void **>(&state.m_particles_mapped))));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, make_error_code(state.m_device->mapBuffer(
                                     state.m_chunk_staging.get(), rhi::CpuAccessMode::Write,
                                     reinterpret_cast<void **>(&state.m_chunk_staging_mapped))));
        state.m_mapped = true;

        // Create roots and bind the shared constants and buffers.
        const auto bind_constants = [&](rhi::IShaderObject *const root) -> std::error_code {
            rhi::ShaderCursor cursor{root}, constants{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, cursor["g_world_light_source"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, constants.setData(&state.m_constants, sizeof(state.m_constants)));
            return {};
        };
        const auto bind_buffer = [&](rhi::IShaderObject *const root, const char *const name,
                                     rhi::IBuffer *const buffer) -> std::error_code {
            rhi::ShaderCursor cursor{root};
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, cursor[name].setBinding(rhi::Binding(buffer, range_(buffer))));
            return {};
        };

        // Upload root: constants + chunk upload + ids.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            state.m_device->createRootShaderObject(state.m_upload_program.get(), state.m_upload_root.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, bind_constants(state.m_upload_root.get()));
        {
            rhi::ShaderCursor cursor{state.m_upload_root.get()}, upload{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, cursor["g_world_light_chunk_upload"].getDereferenced(upload));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, upload.setData(&state.m_upload_constants, sizeof(state.m_upload_constants)));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            bind_buffer(state.m_upload_root.get(), "g_world_light_chunk", state.m_chunk_staging.get()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, bind_buffer(state.m_upload_root.get(), "g_world_light_ids", state.m_ids_gpu.get()));

        // Occupancy root: constants + ids + overlay + occupancy.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            state.m_device->createRootShaderObject(state.m_occupancy_program.get(), state.m_occupancy_root.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, bind_constants(state.m_occupancy_root.get()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, bind_buffer(state.m_occupancy_root.get(), "g_world_light_ids", state.m_ids_gpu.get()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            bind_buffer(state.m_occupancy_root.get(), "g_world_light_overlay", state.m_overlay_gpu.get()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu,
            bind_buffer(state.m_occupancy_root.get(), "g_world_light_occupancy", state.m_occupancy_gpu.get()));

        // Matter roots: constants + particles + materials + overlay.
        const auto bind_matter_root = [&](rhi::IShaderProgram *const program,
                                          rhi::ComPtr<rhi::IShaderObject> &root) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, state.m_device->createRootShaderObject(program, root.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, bind_constants(root.get()));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, bind_buffer(root.get(), "g_world_light_particles", state.m_particles_gpu.get()));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, bind_buffer(root.get(), "g_world_light_materials", state.m_materials_gpu.get()));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, bind_buffer(root.get(), "g_world_light_overlay", state.m_overlay_gpu.get()));
            return {};
        };
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, bind_matter_root(state.m_clear_program.get(), state.m_clear_root));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, bind_matter_root(state.m_scatter_program.get(), state.m_scatter_root));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, bind_matter_root(state.m_finalize_program.get(), state.m_finalize_root));

        state.m_ready = true;
        PPR_LOG(
            WorldLightSourceGpu, info, "WorldLightSourceGpu initialized",
            {{"width", plan.m_width}, {"height", plan.m_height}, {"blocks_x", state.m_blocks_x},
                {"blocks_y", state.m_blocks_y}, {"words_per_row", state.m_words_per_row}});
        return default_value_v;
    }

    std::error_code WorldLightSourceGpu::applyUpdate(const WorldLightSourceUpdate &update) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(state.m_ready, std::errc::not_connected));

        const u32 width = state.m_plan.m_width;
        const u32 height = state.m_plan.m_height;
        const u32 chunks_x = (width + kChunkEdge - 1u) / kChunkEdge;
        const u32 chunks_y = (height + kChunkEdge - 1u) / kChunkEdge;
        const u64 chunk_count = u64{chunks_x} * chunks_y;
        const u64 cell_count = u64{width} * height;

        // Validate the whole edit before mutating anything so a rejected update
        // retains the previous ids, revision, dirty rect, pending work and GPU bytes.
        for (const WorldLightChunkUpdate &chunk: update.m_chunks) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, require_(chunk.m_chunk_id < chunk_count, std::errc::result_out_of_range));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(chunk.m_cell_ids.size() == kChunkCellCount));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightSourceGpu, require_(update.m_particles.size() <= cell_count, std::errc::value_too_large));
        for (const WorldLightMatterParticle &particle: update.m_particles) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, require_(particle.m_cell < cell_count, std::errc::result_out_of_range));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu,
                require_(particle.m_material < state.m_materials.size(), std::errc::result_out_of_range));
        }

        bool changed = false;
        WorldLightDirtyRect dirty;
        std::vector<u32> pending_chunks;

        for (const WorldLightChunkUpdate &chunk: update.m_chunks) {
            const u32 chunk_x = chunk.m_chunk_id % chunks_x;
            const u32 chunk_y = chunk.m_chunk_id / chunks_x;
            const u32 cell_x0 = chunk_x * kChunkEdge;
            const u32 cell_y0 = chunk_y * kChunkEdge;
            const u32 cell_x1 = std::min(cell_x0 + kChunkEdge, width);
            const u32 cell_y1 = std::min(cell_y0 + kChunkEdge, height);

            bool chunk_changed = false;
            for (u32 y = cell_y0; y < cell_y1; ++y) {
                for (u32 x = cell_x0; x < cell_x1; ++x) {
                    const u32 local = (y - cell_y0) * kChunkEdge + (x - cell_x0);
                    const u16 id = chunk.m_cell_ids[local];
                    u16 &slot = state.m_cell_ids[u64{y} * width + x];
                    if (slot != id) {
                        slot = id;
                        chunk_changed = true;
                    }
                }
            }

            if (not chunk_changed) {
                continue;
            }

            changed = true;
            dirty.include(
                static_cast<i32>(cell_x0), static_cast<i32>(cell_y0), static_cast<i32>(cell_x1),
                static_cast<i32>(cell_y1));
            pending_chunks.push_back(chunk.m_chunk_id);
        }

        for (const WorldLightMatterParticle &particle: update.m_particles) {
            changed = true;
            const u32 x = particle.m_cell % width;
            const u32 y = particle.m_cell / width;
            dirty.include(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(x) + 1, static_cast<i32>(y) + 1);
        }

        if (update.m_sky != state.m_sky or update.m_sky_hour != state.m_sky_hour) {
            state.m_sky = update.m_sky;
            state.m_sky_hour = update.m_sky_hour;
            changed = true;
        }

        if (not changed) {
            return default_value_v;
        }

        state.m_dirty = dirty;
        state.m_revision = update.m_revision;

        // The committed particle set replaces the pending set; chunks append.
        state.m_pending_chunks.insert(state.m_pending_chunks.end(), pending_chunks.begin(), pending_chunks.end());
        state.m_pending_particles.assign(update.m_particles.begin(), update.m_particles.end());

        state.m_pending = true;
        return default_value_v;
    }

    std::error_code WorldLightSourceGpu::encode(rhi::ICommandEncoder &encoder) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(state.m_ready, std::errc::not_connected));
        if (not state.m_pending) {
            return default_value_v;
        }

        const u32 width = state.m_plan.m_width;
        const u32 chunks_x = (width + kChunkEdge - 1u) / kChunkEdge;

        // Host-side staging of the widened chunk ids and raw particles.
        for (const u32 chunk_id: state.m_pending_chunks) {
            const u32 chunk_x = chunk_id % chunks_x;
            const u32 chunk_y = chunk_id / chunks_x;
            const u32 cell_x0 = chunk_x * kChunkEdge;
            const u32 cell_y0 = chunk_y * kChunkEdge;
            const u32 cell_x1 = std::min(cell_x0 + kChunkEdge, width);
            const u32 cell_y1 = std::min(cell_y0 + kChunkEdge, state.m_plan.m_height);

            for (u32 y = cell_y0; y < cell_y1; ++y) {
                const u64 row = u64{y} * width;
                const u64 stage_row = u64{y - cell_y0} * kChunkEdge;
                for (u32 x = cell_x0; x < cell_x1; ++x) {
                    state.m_chunk_staging_mapped[stage_row + (x - cell_x0)] = state.m_cell_ids[row + x];
                }
            }

            state.m_upload_constants.m_chunk_x = chunk_x;
            state.m_upload_constants.m_chunk_y = chunk_y;
            rhi::ShaderCursor cursor{state.m_upload_root.get()}, upload{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, cursor["g_world_light_chunk_upload"].getDereferenced(upload));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, upload.setData(&state.m_upload_constants, sizeof(state.m_upload_constants)));

            encoder.setBufferState(state.m_ids_gpu.get(), rhi::ResourceState::UnorderedAccess);
            encoder.setBufferState(state.m_chunk_staging.get(), rhi::ResourceState::ShaderResource);
            auto *const upload_pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(upload_pass != nullptr, std::errc::io_error));
            upload_pass->bindPipeline(state.m_upload_pipeline.get(), state.m_upload_root.get());
            upload_pass->dispatchCompute(static_cast<u32>(kChunkCellCount / kNumthreads), 1u, 1u);
            upload_pass->end();
            encoder.globalBarrier();
        }

        for (u32 index = 0u; index < state.m_pending_particles.size(); ++index) {
            state.m_particles_mapped[index] = state.m_pending_particles[index];
        }

        // Refresh the variable constants before dispatch.
        state.m_constants.m_revision = static_cast<u32>(state.m_revision);
        state.m_constants.m_has_overlay = static_cast<u32>(state.m_pending_particles.size());

        const auto set_constants = [&](rhi::IShaderObject *const root) -> std::error_code {
            rhi::ShaderCursor cursor{root}, constants{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, cursor["g_world_light_source"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightSourceGpu, constants.setData(&state.m_constants, sizeof(state.m_constants)));
            return {};
        };

        const u32 particle_count = static_cast<u32>(state.m_pending_particles.size());

        // Matter projection: clear -> scatter -> finalize over the staged particles.
        if (particle_count != 0u) {
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, set_constants(state.m_clear_root.get()));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, set_constants(state.m_scatter_root.get()));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, set_constants(state.m_finalize_root.get()));

            encoder.setBufferState(state.m_overlay_gpu.get(), rhi::ResourceState::UnorderedAccess);
            encoder.setBufferState(state.m_particles_gpu.get(), rhi::ResourceState::ShaderResource);
            encoder.setBufferState(state.m_materials_gpu.get(), rhi::ResourceState::ShaderResource);

            const u32 groups = (particle_count + kNumthreads - 1u) / kNumthreads;
            for (const auto &[pipeline, root]:
                {std::pair{state.m_clear_pipeline.get(), state.m_clear_root.get()},
                    std::pair{state.m_scatter_pipeline.get(), state.m_scatter_root.get()},
                    std::pair{state.m_finalize_pipeline.get(), state.m_finalize_root.get()}}) {
                auto *const pass = encoder.beginComputePass();
                PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(pass != nullptr, std::errc::io_error));
                pass->bindPipeline(pipeline, root);
                pass->dispatchCompute(groups, 1u, 1u);
                pass->end();
                encoder.globalBarrier();
            }
        }

        // Occupancy rebuild over the complete dense ids + overlay.
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, set_constants(state.m_occupancy_root.get()));
        encoder.setBufferState(state.m_occupancy_gpu.get(), rhi::ResourceState::UnorderedAccess);

        const u64 total_words = u64{state.m_blocks_y} * state.m_words_per_row;
        const u32 occupancy_groups = static_cast<u32>((total_words + kNumthreads - 1u) / kNumthreads);
        auto *const occupancy_pass = encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(WorldLightSourceGpu, require_(occupancy_pass != nullptr, std::errc::io_error));
        occupancy_pass->bindPipeline(state.m_occupancy_pipeline.get(), state.m_occupancy_root.get());
        occupancy_pass->dispatchCompute(occupancy_groups, 1u, 1u);
        occupancy_pass->end();

        state.m_pending_chunks.clear();
        state.m_pending_particles.clear();
        state.m_pending = false;
        return default_value_v;
    }

    rhi::IBuffer *WorldLightSourceGpu::cellIds() const noexcept {
        return m_state->m_ids_gpu.get();
    }

    rhi::IBuffer *WorldLightSourceGpu::overlay() const noexcept {
        return m_state->m_overlay_gpu.get();
    }

    rhi::IBuffer *WorldLightSourceGpu::occupancy() const noexcept {
        return m_state->m_occupancy_gpu.get();
    }

    rhi::IBuffer *WorldLightSourceGpu::materialsGpu() const noexcept {
        return m_state->m_materials_gpu.get();
    }

    std::span<const WorldLightMaterial> WorldLightSourceGpu::materials() const noexcept {
        return m_state->m_materials;
    }

    u64 WorldLightSourceGpu::revision() const noexcept {
        return m_state->m_revision;
    }

    WorldLightDirtyRect WorldLightSourceGpu::dirtyRect() const noexcept {
        return m_state->m_dirty;
    }

    std::error_code WorldLightSourceGpu::shutdown() {
        State &state = *m_state;
        if (state.m_mapped and state.m_device != nullptr) {
            if (state.m_particles_gpu != nullptr) {
                std::ignore = state.m_device->unmapBuffer(state.m_particles_gpu.get());
            }
            if (state.m_chunk_staging != nullptr) {
                std::ignore = state.m_device->unmapBuffer(state.m_chunk_staging.get());
            }
        }

        state.m_particles_mapped = nullptr;
        state.m_chunk_staging_mapped = nullptr;
        state.m_mapped = false;

        state.m_upload_root.setNull();
        state.m_clear_root.setNull();
        state.m_scatter_root.setNull();
        state.m_finalize_root.setNull();
        state.m_occupancy_root.setNull();
        state.m_upload_pipeline.setNull();
        state.m_clear_pipeline.setNull();
        state.m_scatter_pipeline.setNull();
        state.m_finalize_pipeline.setNull();
        state.m_occupancy_pipeline.setNull();
        state.m_upload_program.setNull();
        state.m_clear_program.setNull();
        state.m_scatter_program.setNull();
        state.m_finalize_program.setNull();
        state.m_occupancy_program.setNull();
        state.m_ids_gpu.setNull();
        state.m_overlay_gpu.setNull();
        state.m_occupancy_gpu.setNull();
        state.m_materials_gpu.setNull();
        state.m_particles_gpu.setNull();
        state.m_chunk_staging.setNull();
        state.m_source_module = {};
        state.m_matter_module = {};

        state.m_pending_chunks.clear();
        state.m_pending_particles.clear();
        state.m_pending = false;
        state.m_ready = false;
        state.m_device = nullptr;
        state.m_shaders = nullptr;
        return default_value_v;
    }
} // namespace pP::lighting
