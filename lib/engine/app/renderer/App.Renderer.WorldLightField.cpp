module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;

import :renderer.world_light_field;
import :renderer.world_light_source_gpu;
import :renderer.world_light_types;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP::lighting {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(WorldLightField, info, none)

    // ------------------------------------------------------------------
    // temporal cascade field
    // ------------------------------------------------------------------

    namespace {
        inline constexpr u32 kNumthreads = 64u;
        inline constexpr u32 kRadianceSampleBytes = 8u;

        static_assert(sizeof(WorldLightLevelGpu) == 32u and alignof(WorldLightLevelGpu) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_directions) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_probe_offset) == 8u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_sample_offset) == 12u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_near_m) == 24u);

        static_assert(sizeof(WorldLightFieldConstants) == 316u and alignof(WorldLightFieldConstants) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_level_count) == 16u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_budget) == 32u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_accumulation_samples) == 36u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_history_retention) == 40u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_frame_delta) == 44u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_levels) == 48u);

        // Per-dispatch CPU mirrors of the Slang constant buffers.
        struct WorldLightFieldDispatch {
            u32 m_level{}, m_sample_begin{}, m_sample_count{};
            u32 m_pad0{};
        };
        static_assert(sizeof(WorldLightFieldDispatch) == 16u and alignof(WorldLightFieldDispatch) == 4u);

        struct WorldLightFieldReset {
            u32 m_level{};
            u32 m_probe_x0{}, m_probe_y0{};
            u32 m_rect_width{}, m_rect_height{};
        };
        static_assert(sizeof(WorldLightFieldReset) == 20u and alignof(WorldLightFieldReset) == 4u);

        struct WorldLightFieldProbeUpdate {
            u32 m_level{};
            u32 m_probe_x0{}, m_probe_y0{};
            u32 m_rect_width{}, m_probe_count{};
        };
        static_assert(sizeof(WorldLightFieldProbeUpdate) == 20u and alignof(WorldLightFieldProbeUpdate) == 4u);

        // CPU mirror of the Slang WorldLightEvaluateParams (all-scalar layout to
        // match Slang ConstantBuffer packing; see the field constants note).
        struct WorldLightEvaluateParams {
            float m_position_x{}, m_position_y{};
            float m_direction_x{}, m_direction_y{};
            float m_normal_x{}, m_normal_y{}, m_normal_z{};
            float m_roughness{};
            u32 m_mode{};
            u32 m_pad0{}, m_pad1{}, m_pad2{};
        };
        static_assert(sizeof(WorldLightEvaluateParams) == 48u and alignof(WorldLightEvaluateParams) == 4u);

        std::error_code require_(const bool valid, const std::errc error = std::errc::invalid_argument) noexcept {
            return valid ? std::error_code{} : std::make_error_code(error);
        }

        rhi::BufferRange range_(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }
    }

    struct WorldLightField::State {
        WorldLightPlan m_plan{};
        u64 m_total_samples{};
        u64 m_last_revision{};
        std::array<float, 3> m_last_sky{};
        // One round-robin cursor per cascade level. A single flat cursor spent a
        // whole frame's budget inside one level, so frames alternated between
        // cheap fine-level and expensive coarse-level work; per-level cursors
        // give every level the same refresh rate and a steady per-frame cost.
        std::array<u64, kWorldLightMaxLevels> m_level_cursor{};
        u64 m_cycle{};
        u32 m_current{}; // ping-pong index of the accumulated radiance buffer
        u32 m_frame{};

        rhi::IDevice *m_device{};
        IShaderService *m_shaders{};
        std::filesystem::path m_content_dir;

        shader::SharedModule m_module;
        rhi::ComPtr<rhi::IShaderProgram> m_update_program, m_probe_update_program, m_reset_program;
        rhi::ComPtr<rhi::IComputePipeline> m_update_pipeline, m_probe_update_pipeline, m_reset_pipeline;
        rhi::ComPtr<rhi::IShaderObject> m_update_root, m_probe_update_root, m_reset_root;

        rhi::ComPtr<rhi::IShaderProgram> m_evaluate_program;
        rhi::ComPtr<rhi::IComputePipeline> m_evaluate_pipeline;
        rhi::ComPtr<rhi::IShaderObject> m_evaluate_root;
        rhi::ComPtr<rhi::IBuffer> m_evaluate_output;

        std::array<rhi::ComPtr<rhi::IBuffer>, 2> m_radiance;
        rhi::ComPtr<rhi::IBuffer> m_state;

        WorldLightFieldConstants m_constants{};
        WorldLightFieldDispatch m_dispatch{};
        WorldLightFieldReset m_reset{};
        WorldLightFieldProbeUpdate m_probe_update{};
        WorldLightEvaluateParams m_evaluate_params{};

        bool m_ready{};
    };

    WorldLightField::WorldLightField() : m_state{std::make_unique<State>()} {
    }

    WorldLightField::~WorldLightField() {
        std::ignore = shutdown();
    }

    std::error_code WorldLightField::initialize(
        IRhiService &rhi, IShaderService &shaders, const WorldLightPlan &plan,
        const std::filesystem::path &content_dir) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(not state.m_ready, std::errc::already_connected));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(plan.m_width != 0u and plan.m_height != 0u));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(plan.m_radiance_samples != 0u));

        state.m_device = &rhi.getDevice();
        state.m_shaders = &shaders;
        state.m_content_dir = content_dir;
        state.m_plan = plan;
        state.m_total_samples = plan.m_radiance_samples;
        state.m_current = 0u;
        state.m_frame = 0u;

        // Fill the field constants and per-level GPU mirrors.
        state.m_constants.m_origin_x = 0.0f;
        state.m_constants.m_origin_y = 0.0f;
        state.m_constants.m_width = plan.m_width;
        state.m_constants.m_height = plan.m_height;
        state.m_constants.m_level_count = plan.m_level_count;
        state.m_constants.m_cell_metres = plan.m_cell_metres;

        u64 probe_offset = 0u;
        for (u32 level = 0u; level < plan.m_level_count; ++level) {
            const WorldLightLevel &input = plan.m_levels[level];
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, require_(input.m_sample_offset <= std::numeric_limits<u32>::max()));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(probe_offset <= std::numeric_limits<u32>::max()));

            WorldLightLevelGpu &gpu = state.m_constants.m_levels[level];
            gpu.m_spacing = input.m_spacing_cells;
            gpu.m_directions = input.m_directions;
            gpu.m_probe_offset = static_cast<u32>(probe_offset);
            gpu.m_sample_offset = static_cast<u32>(input.m_sample_offset);
            gpu.m_probes_x = input.m_probes_x;
            gpu.m_probes_y = input.m_probes_y;
            gpu.m_near_m = input.m_interval_near_m;
            gpu.m_far_m = input.m_interval_far_m;

            probe_offset += u64{input.m_probes_x} * input.m_probes_y;
        }

        // Load the field module and build the three compute programs/pipelines.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, shaders.loadModuleFromFile(
                                 content_dir / TEXT("shaders") / TEXT("world_light_trace.slang"), "world_light_trace",
                                 state.m_module.writeRef()));

        const auto build_program = [&](const char *const name, rhi::ComPtr<rhi::IShaderProgram> &program,
                                       rhi::ComPtr<rhi::IComputePipeline> &pipeline) -> std::error_code {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, state.m_module->findEntryPointByName(name, entry.writeRef()));

            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = state.m_module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;

            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField,
                state.m_device->createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline_desc{};
            pipeline_desc.program = program;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, state.m_device->createComputePipeline(pipeline_desc, pipeline.writeRef()));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, build_program("updateWorldLightField", state.m_update_program, state.m_update_pipeline));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField,
            build_program("updateWorldLightFieldProbe", state.m_probe_update_program, state.m_probe_update_pipeline));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, build_program("resetWorldLightField", state.m_reset_program, state.m_reset_pipeline));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField,
            build_program("evaluateWorldLightField", state.m_evaluate_program, state.m_evaluate_pipeline));

        // Double-buffered radiance (8 B/sample) and per-direction counts (u32,
        // widened from the plan's u8 estimate so each thread owns one word).
        const u64 radiance_bytes = plan.m_radiance_bytes;
        const u64 state_bytes = plan.m_radiance_samples * sizeof(u32);
        const std::vector<u32> zero_radiance(radiance_bytes / sizeof(u32), 0u);
        const std::vector<u32> zero_state(plan.m_radiance_samples, 0u);

        const auto device_buffer = [&](const u64 bytes, const u32 stride, const void *const initial,
                                       const char *const label, rhi::ComPtr<rhi::IBuffer> &out) -> std::error_code {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = stride;
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource) |
                static_cast<u32>(rhi::BufferUsage::CopyDestination));
            description.defaultState = rhi::ResourceState::UnorderedAccess;
            description.label = label;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, state.m_device->createBuffer(description, initial, out.writeRef()));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, device_buffer(
                                 radiance_bytes, kRadianceSampleBytes, zero_radiance.data(),
                                 "world light field radiance 0", state.m_radiance[0]));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, device_buffer(
                                 radiance_bytes, kRadianceSampleBytes, zero_radiance.data(),
                                 "world light field radiance 1", state.m_radiance[1]));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField,
            device_buffer(state_bytes, sizeof(u32), zero_state.data(), "world light field state", state.m_state));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, device_buffer(
                                 3u * sizeof(float), sizeof(float), nullptr, "world light field evaluate output",
                                 state.m_evaluate_output));

        // Create the three roots. Bindings change each update (ping-pong and the
        // borrowed source buffers), so they are set in update().
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField,
            state.m_device->createRootShaderObject(state.m_update_program.get(), state.m_update_root.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, state.m_device->createRootShaderObject(
                                 state.m_probe_update_program.get(), state.m_probe_update_root.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField,
            state.m_device->createRootShaderObject(state.m_reset_program.get(), state.m_reset_root.writeRef()));

        // Evaluate root: the field constants and the output buffer are static; the
        // params and the current radiance buffer are (re)bound in evaluate().
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField,
            state.m_device->createRootShaderObject(state.m_evaluate_program.get(), state.m_evaluate_root.writeRef()));
        {
            rhi::ShaderCursor cursor{state.m_evaluate_root.get()}, constants{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_field"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, constants.setData(&state.m_constants, sizeof(state.m_constants)));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, rhi::ShaderCursor{state.m_evaluate_root.get()}["g_world_light_evaluate_out"].setBinding(
                                 rhi::Binding(state.m_evaluate_output.get(), range_(state.m_evaluate_output.get()))));

        state.m_ready = true;
        PPR_LOG(
            WorldLightField, info, "WorldLightField initialized",
            {{"width", plan.m_width}, {"height", plan.m_height}, {"levels", plan.m_level_count},
                {"samples", plan.m_radiance_samples}, {"radiance_bytes", plan.m_radiance_bytes}});
        return default_value_v;
    }

    std::error_code WorldLightField::update(
        rhi::ICommandEncoder &encoder, const WorldLightSourceGpu &source, rhi::IBuffer &atlas, const float dt,
        const u32 sample_budget, const u32 accumulation_samples, const float history_retention) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(state.m_ready, std::errc::not_connected));

        encoder.pushDebugGroup("WorldLight.FieldUpdate", rhi::MarkerColor{.r = 0.9f, .g = 0.6f, .b = 0.1f});
        PPR_DEFER {
            encoder.popDebugGroup();
        };

        const u64 total = state.m_total_samples;
        const u32 budget = static_cast<u32>(std::min<u64>(sample_budget, total));

        ++state.m_frame;
        state.m_constants.m_frame = state.m_frame;
        state.m_constants.m_budget = budget;
        state.m_constants.m_source_revision = static_cast<u32>(source.revision());
        state.m_constants.m_accumulation_samples = std::max(1u, accumulation_samples);
        state.m_constants.m_history_retention = std::clamp(history_retention, 0.0f, 1.0f);
        state.m_constants.m_frame_delta = dt;

        const std::array<float, 3> sky = source.sky();
        state.m_constants.m_sky_r = sky[0];
        state.m_constants.m_sky_g = sky[1];
        state.m_constants.m_sky_b = sky[2];

        const u32 read_index = state.m_current;
        const u32 write_index = 1u - state.m_current;

        // Bind the shared constants, the ping-pong radiance buffers, the state
        // buffer and the borrowed dense source buffers on all three roots.
        const auto bind_root = [&](rhi::IShaderObject *const root) -> std::error_code {
            rhi::ShaderCursor cursor{root}, constants{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_field"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, constants.setData(&state.m_constants, sizeof(state.m_constants)));

            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField,
                cursor["g_world_light_radiance_prev"].setBinding(
                    rhi::Binding(state.m_radiance[read_index].get(), range_(state.m_radiance[read_index].get()))));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField,
                cursor["g_world_light_radiance_cur"].setBinding(
                    rhi::Binding(state.m_radiance[write_index].get(), range_(state.m_radiance[write_index].get()))));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, cursor["g_world_light_state"].setBinding(
                                     rhi::Binding(state.m_state.get(), range_(state.m_state.get()))));

            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField,
                cursor["g_world_light_ids"].setBinding(rhi::Binding(source.cellIds(), range_(source.cellIds()))));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField,
                cursor["g_world_light_overlay"].setBinding(rhi::Binding(source.overlay(), range_(source.overlay()))));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, cursor["g_world_light_materials"].setBinding(
                                     rhi::Binding(source.materialsGpu(), range_(source.materialsGpu()))));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, cursor["g_world_light_atlas"].setBinding(rhi::Binding(&atlas, range_(&atlas))));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_root(state.m_update_root.get()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_root(state.m_probe_update_root.get()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_root(state.m_reset_root.get()));

        // Borrowed source buffers (ids/overlay are RW; materials is SR), the
        // packed radiance atlas (written by its own pass earlier in the command
        // buffer, read here), and the field's own ping-pong radiance/state
        // buffers written this frame.
        encoder.setBufferState(source.cellIds(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(source.overlay(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(source.materialsGpu(), rhi::ResourceState::ShaderResource);
        encoder.setBufferState(&atlas, rhi::ResourceState::ShaderResource);
        encoder.setBufferState(state.m_radiance[read_index].get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_radiance[write_index].get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_state.get(), rhi::ResourceState::UnorderedAccess);

        // Copy the previous field into the current buffer so the round-robin
        // update only overwrites the traced range and the current buffer stays a
        // complete field after the swap.
        encoder.pushDebugGroup("WorldLight.FieldCopy", rhi::MarkerColor{.r = 0.8f, .g = 0.2f, .b = 0.2f});
        encoder.setBufferState(state.m_radiance[read_index].get(), rhi::ResourceState::CopySource);
        encoder.setBufferState(state.m_radiance[write_index].get(), rhi::ResourceState::CopyDestination);
        encoder.copyBuffer(
            state.m_radiance[write_index].get(), 0u, state.m_radiance[read_index].get(), 0u,
            state.m_plan.m_radiance_bytes);
        encoder.setBufferState(state.m_radiance[read_index].get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_radiance[write_index].get(), rhi::ResourceState::UnorderedAccess);
        encoder.globalBarrier();
        encoder.popDebugGroup();

        // Per-level probe rectangle of the source dirty rect in probe grid units.
        struct ProbeRect {
            u32 m_x0{}, m_y0{}, m_x1{}, m_y1{};
        };
        const auto probe_rect = [&](const WorldLightLevel &info,
                                    const WorldLightDirtyRect &dirty) -> std::optional<ProbeRect> {
            const u32 spacing = info.m_spacing_cells;
            const u32 px0 = static_cast<u32>(std::max(0, dirty.m_min_x)) / spacing;
            const u32 py0 = static_cast<u32>(std::max(0, dirty.m_min_y)) / spacing;
            const u32 px1 = std::min(
                info.m_probes_x, (static_cast<u32>(std::max(0, dirty.m_max_x)) + spacing - 1u) / spacing);
            const u32 py1 = std::min(
                info.m_probes_y, (static_cast<u32>(std::max(0, dirty.m_max_y)) + spacing - 1u) / spacing);
            if (px0 >= px1 or py0 >= py1) {
                return std::nullopt;
            }
            return ProbeRect{px0, py0, px1, py1};
        };

        const auto dispatch_update = [&](const u32 level, const u64 local_begin,
                                         const u64 local_count) -> std::error_code {
            if (local_count == 0u) {
                return {};
            }
            state.m_dispatch.m_level = level;
            state.m_dispatch.m_sample_begin = static_cast<u32>(local_begin);
            state.m_dispatch.m_sample_count = static_cast<u32>(local_count);

            rhi::ShaderCursor cursor{state.m_update_root.get()}, dispatch{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_dispatch"].getDereferenced(dispatch));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, dispatch.setData(&state.m_dispatch, sizeof(state.m_dispatch)));

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_update_pipeline.get(), state.m_update_root.get());
            pass->dispatchCompute(static_cast<u32>((local_count + kNumthreads - 1u) / kNumthreads), 1u, 1u);
            pass->end();
            return {};
        };

        const auto dispatch_reset = [&](const u32 level, const u32 px0, const u32 py0, const u32 rect_width,
                                        const u32 rect_height) -> std::error_code {
            state.m_reset.m_level = level;
            state.m_reset.m_probe_x0 = px0;
            state.m_reset.m_probe_y0 = py0;
            state.m_reset.m_rect_width = rect_width;
            state.m_reset.m_rect_height = rect_height;

            rhi::ShaderCursor cursor{state.m_reset_root.get()}, reset{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_reset"].getDereferenced(reset));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, reset.setData(&state.m_reset, sizeof(state.m_reset)));

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_reset_pipeline.get(), state.m_reset_root.get());
            pass->dispatchCompute(
                static_cast<u32>((u64{rect_width} * rect_height + kNumthreads - 1u) / kNumthreads), 1u, 1u);
            pass->end();
            return {};
        };

        const auto dispatch_probe_update = [&](const u32 level, const u32 px0, const u32 py0, const u32 rect_width,
                                               const u32 probe_count) -> std::error_code {
            state.m_probe_update.m_level = level;
            state.m_probe_update.m_probe_x0 = px0;
            state.m_probe_update.m_probe_y0 = py0;
            state.m_probe_update.m_rect_width = rect_width;
            state.m_probe_update.m_probe_count = probe_count;

            rhi::ShaderCursor cursor{state.m_probe_update_root.get()}, update{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_probe_update"].getDereferenced(update));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, update.setData(&state.m_probe_update, sizeof(state.m_probe_update)));

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_probe_update_pipeline.get(), state.m_probe_update_root.get());
            pass->dispatchCompute(static_cast<u32>((u64{probe_count} + kNumthreads - 1u) / kNumthreads), 1u, 1u);
            pass->end();
            return {};
        };

        // Dirty reset, then dirty-first reprocess, then the round-robin cursor
        // with whatever budget remains. The reset runs before the reprocess so an
        // edit is visible in the same frame, not deferred to the next cursor wrap.
        encoder.pushDebugGroup("WorldLight.FieldTrace", rhi::MarkerColor{.r = 0.9f, .g = 0.6f, .b = 0.1f});
        PPR_DEFER {
            encoder.popDebugGroup();
        };
        u64 remaining = budget;
        if (source.revision() != state.m_last_revision) {
            encoder.pushDebugGroup("WorldLight.FieldDirty", rhi::MarkerColor{.r = 0.9f, .g = 0.2f, .b = 0.9f});
            PPR_DEFER {
                encoder.popDebugGroup();
            };
            const WorldLightDirtyRect dirty = source.dirtyRect();
            if (not dirty.empty()) {
                for (u32 level = 0u; level < state.m_plan.m_level_count; ++level) {
                    const auto rect = probe_rect(state.m_plan.m_levels[level], dirty);
                    if (not rect) {
                        continue;
                    }
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldLightField,
                        dispatch_reset(
                            level, rect->m_x0, rect->m_y0, rect->m_x1 - rect->m_x0, rect->m_y1 - rect->m_y0));
                }
                encoder.globalBarrier();

                for (u32 level = 0u; level < state.m_plan.m_level_count and remaining != 0u; ++level) {
                    const auto rect = probe_rect(state.m_plan.m_levels[level], dirty);
                    if (not rect) {
                        continue;
                    }
                    const WorldLightLevel &info = state.m_plan.m_levels[level];
                    const u32 rect_width = rect->m_x1 - rect->m_x0;
                    const u64 dirty_samples = u64{rect_width} * (rect->m_y1 - rect->m_y0) * info.m_directions;
                    const u32 probe_count = static_cast<u32>(
                        std::min<u64>(dirty_samples, remaining) / info.m_directions);
                    if (probe_count == 0u) {
                        break;
                    }
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldLightField, dispatch_probe_update(level, rect->m_x0, rect->m_y0, rect_width, probe_count));
                    remaining -= u64{probe_count} * info.m_directions;
                }
                encoder.globalBarrier();
            }

            // A sky change dirties only the coarsest cascade level: an escaped
            // ray's terminal contribution changes there, and the lower levels must
            // keep their history. Reset and reprocess the full coarsest probe grid,
            // bounded by the same remaining budget.
            if (sky != state.m_last_sky) {
                const WorldLightLevel &coarsest = state.m_plan.m_levels[state.m_plan.m_level_count - 1u];
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightField,
                    dispatch_reset(state.m_plan.m_level_count - 1u, 0u, 0u, coarsest.m_probes_x, coarsest.m_probes_y));
                encoder.globalBarrier();

                const u64 sky_samples = u64{coarsest.m_probes_x} * coarsest.m_probes_y * coarsest.m_directions;
                const u32 sky_probe_count = static_cast<u32>(
                    std::min<u64>(sky_samples, remaining) / coarsest.m_directions);
                if (sky_probe_count != 0u) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldLightField,
                        dispatch_probe_update(
                            state.m_plan.m_level_count - 1u, 0u, 0u, coarsest.m_probes_x, sky_probe_count));
                    remaining -= u64{sky_probe_count} * coarsest.m_directions;
                }
                encoder.globalBarrier();
            }

            state.m_last_revision = source.revision();
            state.m_last_sky = sky;
        }

        // Round-robin refresh with the remaining budget, split evenly across the
        // cascade levels. Every level stores the same sample count, so an even
        // split refreshes all scales at the same rate and keeps the per-frame
        // cost steady instead of alternating cheap fine-level and expensive
        // coarse-level frames.
        const u64 level_count = std::max<u64>(1u, state.m_plan.m_level_count);
        const u64 level_share = remaining / level_count;
        const u64 level_remainder = remaining % level_count;
        for (u32 level = 0u; level < state.m_plan.m_level_count and remaining != 0u; ++level) {
            const WorldLightLevel &info = state.m_plan.m_levels[level];
            const u64 quota = std::min(
                {level_share + (u64{level} < level_remainder ? 1u : 0u), remaining, info.m_sample_count});
            if (quota == 0u) {
                continue;
            }

            const u64 start = state.m_level_cursor[level];
            const u64 end = start + quota;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, dispatch_update(level, start, std::min(quota, info.m_sample_count - start)));
            if (end > info.m_sample_count) {
                PPR_RETURN_ERROR_ON_FAIL(WorldLightField, dispatch_update(level, 0u, end - info.m_sample_count));
            }

            state.m_level_cursor[level] = end % info.m_sample_count;
            remaining -= quota;

            // One cycle is one complete pass over the finest level, matching the
            // previous single-cursor meaning used for reporting and tests.
            if (level == 0u and end >= info.m_sample_count) {
                ++state.m_cycle;
            }
        }
        encoder.globalBarrier();

        state.m_current = write_index;
        return default_value_v;
    }

    WorldLightFieldView WorldLightField::view() const noexcept {
        const State &state = *m_state;
        const u32 current = state.m_current;
        return {
            .m_radiance = state.m_radiance[current].get(),
            .m_previous_radiance = state.m_radiance[1u - current].get(),
            .m_constants = &state.m_constants,
        };
    }

    u64 WorldLightField::cycle() const noexcept {
        return m_state->m_cycle;
    }

    rhi::IBuffer *WorldLightField::state() const noexcept {
        return m_state->m_state.get();
    }

    std::error_code WorldLightField::evaluate(
        rhi::ICommandEncoder &encoder, const float2 &position, const float2 &direction, const float3 &normal,
        const float roughness, const EvaluateMode mode) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(state.m_ready, std::errc::not_connected));

        state.m_evaluate_params.m_position_x = position.x;
        state.m_evaluate_params.m_position_y = position.y;
        state.m_evaluate_params.m_direction_x = direction.x;
        state.m_evaluate_params.m_direction_y = direction.y;
        state.m_evaluate_params.m_normal_x = normal.x;
        state.m_evaluate_params.m_normal_y = normal.y;
        state.m_evaluate_params.m_normal_z = normal.z;
        state.m_evaluate_params.m_roughness = roughness;
        state.m_evaluate_params.m_mode = static_cast<u32>(mode);

        // Rebind the params and the current radiance buffer; the field constants
        // and the output buffer were bound once in initialize().
        {
            rhi::ShaderCursor cursor{state.m_evaluate_root.get()}, params{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_evaluate"].getDereferenced(params));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, params.setData(&state.m_evaluate_params, sizeof(state.m_evaluate_params)));
        }
        const u32 current = state.m_current;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField,
            rhi::ShaderCursor{state.m_evaluate_root.get()}["g_world_light_radiance_cur"].setBinding(
                rhi::Binding(state.m_radiance[current].get(), range_(state.m_radiance[current].get()))));

        encoder.setBufferState(state.m_radiance[current].get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_evaluate_output.get(), rhi::ResourceState::UnorderedAccess);

        auto *const pass = encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(pass != nullptr, std::errc::io_error));
        pass->bindPipeline(state.m_evaluate_pipeline.get(), state.m_evaluate_root.get());
        pass->dispatchCompute(1u, 1u, 1u);
        pass->end();
        return default_value_v;
    }

    rhi::IBuffer *WorldLightField::evaluateOutput() const noexcept {
        return m_state->m_evaluate_output.get();
    }

    u64 WorldLightField::cursor() const noexcept {
        // The finest level's round-robin position; every level advances at the
        // same rate, so this is representative of refresh progress.
        return m_state->m_level_cursor[0];
    }

    std::error_code WorldLightField::shutdown() {
        State &state = *m_state;

        state.m_update_root.setNull();
        state.m_probe_update_root.setNull();
        state.m_reset_root.setNull();
        state.m_update_pipeline.setNull();
        state.m_probe_update_pipeline.setNull();
        state.m_reset_pipeline.setNull();
        state.m_update_program.setNull();
        state.m_probe_update_program.setNull();
        state.m_reset_program.setNull();
        state.m_evaluate_root.setNull();
        state.m_evaluate_pipeline.setNull();
        state.m_evaluate_program.setNull();
        state.m_evaluate_output.setNull();
        state.m_radiance[0].setNull();
        state.m_radiance[1].setNull();
        state.m_state.setNull();
        state.m_module = {};

        state.m_ready = false;
        state.m_device = nullptr;
        state.m_shaders = nullptr;
        return default_value_v;
    }
} // namespace pP::lighting
