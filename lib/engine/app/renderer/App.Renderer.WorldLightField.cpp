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
        inline constexpr u32 kNumthreads = 128u;
        inline constexpr u32 kRadianceSampleBytes = 8u;

        static_assert(sizeof(WorldLightLevelGpu) == 32u and alignof(WorldLightLevelGpu) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_directions) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_probe_offset) == 8u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_sample_offset) == 12u);
        static_assert(PPR_OFFSETOF(WorldLightLevelGpu, m_near_m) == 24u);

        static_assert(sizeof(WorldLightFieldConstants) == 312u and alignof(WorldLightFieldConstants) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_level_count) == 16u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_budget) == 32u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_frame_delta) == 36u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_levels) == 48u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_sky_hour) == 304u);
        static_assert(PPR_OFFSETOF(WorldLightFieldConstants, m_sky_intensity) == 308u);

        // Per-dispatch CPU mirrors of the Slang constant buffers.
        struct WorldLightFieldDispatch {
            u32 m_level{}, m_group_begin{}, m_group_count{}, m_phase{};
        };
        static_assert(sizeof(WorldLightFieldDispatch) == 16u and alignof(WorldLightFieldDispatch) == 4u);

        struct WorldLightFieldReset {
            u32 m_level{};
            u32 m_probe_x0{}, m_probe_y0{};
            u32 m_rect_width{}, m_rect_height{};
        };
        static_assert(sizeof(WorldLightFieldReset) == 20u and alignof(WorldLightFieldReset) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightFieldReset, m_rect_width) == 12u);
        static_assert(PPR_OFFSETOF(WorldLightFieldReset, m_rect_height) == 16u);

        struct WorldLightFieldProbeUpdate {
            u32 m_level{};
            u32 m_probe_x0{}, m_probe_y0{};
            u32 m_rect_width{}, m_sample_count{};
        };
        static_assert(sizeof(WorldLightFieldProbeUpdate) == 20u and alignof(WorldLightFieldProbeUpdate) == 4u);
        static_assert(PPR_OFFSETOF(WorldLightFieldProbeUpdate, m_rect_width) == 12u);
        static_assert(PPR_OFFSETOF(WorldLightFieldProbeUpdate, m_sample_count) == 16u);

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

        // Weighted ray cost of one stored direction sample: a coarsest sample
        // traces four sky sub-rays; every finer sample traces four per-parent
        // connector rays. Both cost four rays per stored direction sample.
        [[nodiscard]] constexpr u64 levelCost_(const u32 level, const u32 level_count) noexcept {
            (void) level;
            (void) level_count;
            return 4u;
        }

        [[nodiscard]] constexpr u32 nextPow2_(u32 value) noexcept {
            u32 result = 1u;
            while (result < value) {
                result <<= 1u;
            }
            return result;
        }

        [[nodiscard]] constexpr u32 log2Exact_(const u32 value) noexcept {
            u32 bits = 0u;
            u32 remaining = value;
            while (remaining > 1u) {
                remaining >>= 1u;
                ++bits;
            }
            return bits;
        }

        struct GroupDomain {
            u32 m_pad_x{};
            u32 m_pad_y{};
            u32 m_bits_x{};
            u32 m_bits_y{};
            u64 m_count{};
        };

        [[nodiscard]] GroupDomain groupDomain_(const WorldLightLevel &info, const u32 level) noexcept {
            const u32 probe_size = 2u << level;
            const u32 group_width = (info.m_probes_x * probe_size) / 2u;
            const u32 group_height = (info.m_probes_y * probe_size) / 2u;
            GroupDomain domain{};
            domain.m_pad_x = nextPow2_(std::max(1u, group_width));
            domain.m_pad_y = nextPow2_(std::max(1u, group_height));
            domain.m_bits_x = log2Exact_(domain.m_pad_x);
            domain.m_bits_y = log2Exact_(domain.m_pad_y);
            domain.m_count = u64{domain.m_pad_x} * domain.m_pad_y;
            return domain;
        }
    }

    struct WorldLightField::State {
        WorldLightPlan m_plan{};
        u64 m_last_revision{};
        i32 m_last_sky_hour{};
        float m_last_sky_intensity{};
        // Persistent per-level packed-group cursor (bounded by the padded group
        // domain) and four-sweep phase. Dirty/sky work is capped at half the
        // frame budget; the reserved remainder advances these cursors so
        // repeated revisions cannot starve any level or sample. Counts are
        // diagnostic visit metadata; zero is the dirty/fresh backlog sentinel.
        std::array<u64, kWorldLightMaxLevels> m_level_cursor{};
        std::array<u32, kWorldLightMaxLevels> m_level_phase{};
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

        // Borrowed source buffers and atlas from the last update, published in
        // view() so the terrain shading can run the in-place near trace through
        // the dense source. The owners outlive the view; a not-yet-updated field
        // leaves them null and consumers fall back to the legacy path.
        rhi::IBuffer *m_source_cell_ids{};
        rhi::IBuffer *m_source_overlay{};
        rhi::IBuffer *m_source_materials{};
        rhi::IBuffer *m_source_atlas{};

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
        state.m_current = 0u;
        state.m_frame = 0u;
        state.m_cycle = 0u;
        state.m_level_cursor.fill(0u);
        state.m_level_phase.fill(0u);
        // The source starts with the sky disabled (-1 hour); seed the change
        // detector with that state so the first update is not a spurious reset.
        state.m_last_sky_hour = -1;
        state.m_last_sky_intensity = plan.m_sky_intensity;

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

        // Evaluate root: the output buffer is static; the current field
        // constants, params and radiance buffer are (re)bound in evaluate().
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
        const u32 sample_budget, rhi::IQueryPool *const timing) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(state.m_ready, std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightField, require_(std::isfinite(dt) and dt >= 0.0f, std::errc::invalid_argument));

        encoder.pushDebugGroup("WorldLight.FieldUpdate", rhi::MarkerColor{.r = 0.9f, .g = 0.6f, .b = 0.1f});
        PPR_DEFER {
            encoder.popDebugGroup();
        };

        const u32 level_count = state.m_plan.m_level_count;
        u64 weighted_total = 0u;
        for (u32 level = 0u; level < level_count; ++level) {
            weighted_total += state.m_plan.m_levels[level].m_sample_count * levelCost_(level, level_count);
        }
        // Budget units are rays: every stored direction sample costs four (four
        // sky sub-rays at the coarsest level, four per-parent-corner connectors
        // elsewhere). The cap is the work needed to trace every stored sample
        // once, not the stored sample count, so an oversized test budget still
        // fills the field.
        const u64 budget = std::min<u64>(sample_budget, weighted_total);

        ++state.m_frame;
        state.m_constants.m_frame = state.m_frame;
        state.m_constants.m_budget = static_cast<u32>(budget);
        state.m_constants.m_source_revision = static_cast<u32>(source.revision());
        state.m_constants.m_frame_delta = dt;

        state.m_constants.m_sky_hour = static_cast<float>(source.skyHour());
        state.m_constants.m_sky_intensity = state.m_plan.m_sky_intensity;

        // Publish the traced source for this frame's consumers (terrain shading
        // binds them read-only for the in-place near trace).
        state.m_source_cell_ids = source.cellIds();
        state.m_source_overlay = source.overlay();
        state.m_source_materials = source.materialsGpu();
        state.m_source_atlas = &atlas;

        const u32 read_index = state.m_current;
        const u32 write_index = 1u - state.m_current;

        // Bind the shared constants, the ping-pong radiance buffers, the state
        // buffer and the borrowed dense source buffers on all three roots. The
        // trace reads the dense ids/overlay through the read-only descriptors
        // (g_world_light_dense_*); the write descriptors stay bound for the
        // source ABI's sake and are unused by these entry points.
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
                WorldLightField,
                cursor["g_world_light_dense_ids"].setBinding(rhi::Binding(source.cellIds(), range_(source.cellIds()))));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, cursor["g_world_light_dense_overlay"].setBinding(
                                     rhi::Binding(source.overlay(), range_(source.overlay()))));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, cursor["g_world_light_atlas"].setBinding(rhi::Binding(&atlas, range_(&atlas))));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_root(state.m_update_root.get()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_root(state.m_probe_update_root.get()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_root(state.m_reset_root.get()));

        // Borrowed source buffers (read-only for the trace and the terrain's
        // in-place near trace), the packed radiance atlas (written by its own
        // pass earlier in the command buffer, read here), and the field's own
        // ping-pong radiance/state buffers written this frame.
        encoder.setBufferState(source.cellIds(), rhi::ResourceState::ShaderResource);
        encoder.setBufferState(source.overlay(), rhi::ResourceState::ShaderResource);
        encoder.setBufferState(source.materialsGpu(), rhi::ResourceState::ShaderResource);
        encoder.setBufferState(&atlas, rhi::ResourceState::ShaderResource);
        encoder.setBufferState(state.m_radiance[read_index].get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_radiance[write_index].get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_state.get(), rhi::ResourceState::UnorderedAccess);

        if (timing) {
            encoder.writeTimestamp(timing, 0u);
        }

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
        if (timing) {
            encoder.writeTimestamp(timing, 1u);
        }

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

        const auto dispatch_update = [&](const u32 level, const u64 group_begin, const u64 group_count,
                                         const u32 phase) -> std::error_code {
            if (group_count == 0u) {
                return {};
            }
            state.m_dispatch.m_level = level;
            state.m_dispatch.m_group_begin = static_cast<u32>(group_begin);
            state.m_dispatch.m_group_count = static_cast<u32>(group_count);
            state.m_dispatch.m_phase = phase;

            rhi::ShaderCursor cursor{state.m_update_root.get()}, dispatch{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_dispatch"].getDereferenced(dispatch));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, dispatch.setData(&state.m_dispatch, sizeof(state.m_dispatch)));

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_update_pipeline.get(), state.m_update_root.get());
            pass->dispatchCompute(static_cast<u32>((group_count + kNumthreads - 1u) / kNumthreads), 1u, 1u);
            pass->end();
            return {};
        };

        const auto dispatch_reset = [&](const u32 level, const ProbeRect &rect) -> std::error_code {
            state.m_reset.m_level = level;
            state.m_reset.m_probe_x0 = rect.m_x0;
            state.m_reset.m_probe_y0 = rect.m_y0;
            state.m_reset.m_rect_width = rect.m_x1 - rect.m_x0;
            state.m_reset.m_rect_height = rect.m_y1 - rect.m_y0;

            rhi::ShaderCursor cursor{state.m_reset_root.get()}, reset{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_reset"].getDereferenced(reset));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, reset.setData(&state.m_reset, sizeof(state.m_reset)));

            const u64 samples = u64{state.m_reset.m_rect_width} *
                                state.m_reset.m_rect_height *
                                state.m_plan.m_levels[level].m_directions;
            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_reset_pipeline.get(), state.m_reset_root.get());
            pass->dispatchCompute(static_cast<u32>((samples + kNumthreads - 1u) / kNumthreads), 1u, 1u);
            pass->end();
            return {};
        };

        const auto dispatch_probe_update = [&](const u32 level, const ProbeRect &rect,
                                               const u64 sample_count) -> std::error_code {
            if (sample_count == 0u) {
                return {};
            }
            state.m_probe_update.m_level = level;
            state.m_probe_update.m_probe_x0 = rect.m_x0;
            state.m_probe_update.m_probe_y0 = rect.m_y0;
            state.m_probe_update.m_rect_width = rect.m_x1 - rect.m_x0;
            state.m_probe_update.m_sample_count = static_cast<u32>(sample_count);

            rhi::ShaderCursor cursor{state.m_probe_update_root.get()}, update{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_probe_update"].getDereferenced(update));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, update.setData(&state.m_probe_update, sizeof(state.m_probe_update)));

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_probe_update_pipeline.get(), state.m_probe_update_root.get());
            pass->dispatchCompute(static_cast<u32>((sample_count + kNumthreads - 1u) / kNumthreads), 1u, 1u);
            pass->end();
            return {};
        };

        // Weighted level allocation. Every stored direction sample costs four
        // rays (four sky sub-rays at the coarsest level, four connector rays
        // elsewhere). A non-zero budget below four therefore traces nothing: one
        // stored sample is the atomic unit. Every active level receives the same
        // nominal direction-sample quota, then leftover work is redistributed from
        // a frame-rotating start so rounding and per-level caps cannot permanently
        // starve a level.
        const auto allocate_quotas = [&](const u64 work, const std::array<u32, kWorldLightMaxLevels> &levels,
                                         const u32 active_count, const std::array<u64, kWorldLightMaxLevels> &capacity,
                                         std::array<u64, kWorldLightMaxLevels> &quotas) noexcept {
            quotas.fill(0u);
            if (work == 0u or active_count == 0u) {
                return;
            }

            u64 weight_sum = 0u;
            for (u32 index = 0u; index < active_count; ++index) {
                weight_sum += levelCost_(levels[index], level_count);
            }

            const u64 nominal = work / weight_sum;
            u64 used = 0u;
            for (u32 index = 0u; index < active_count; ++index) {
                const u32 level = levels[index];
                const u64 quota = std::min(nominal, capacity[level]);
                quotas[level] = quota;
                used += quota * levelCost_(level, level_count);
            }

            u64 remaining = work - used;
            const u32 start = static_cast<u32>(state.m_frame % active_count);
            if (nominal == 0u) {
                // Tiny budget: rotate one direction sample at a time so every
                // level receives service instead of the first level absorbing the
                // whole remainder.
                bool progress = true;
                while (remaining != 0u and progress) {
                    progress = false;
                    for (u32 offset = 0u; offset < active_count; ++offset) {
                        const u32 level = levels[(start + offset) % active_count];
                        const u64 cost = levelCost_(level, level_count);
                        if (remaining < cost or quotas[level] >= capacity[level]) {
                            continue;
                        }
                        ++quotas[level];
                        remaining -= cost;
                        progress = true;
                    }
                }
                return;
            }

            // Larger leftovers come from capped levels; move them to levels that
            // still have capacity, starting at the rotating level.
            bool progress = true;
            while (remaining != 0u and progress) {
                progress = false;
                for (u32 offset = 0u; offset < active_count; ++offset) {
                    const u32 level = levels[(start + offset) % active_count];
                    const u64 cost = levelCost_(level, level_count);
                    if (remaining < cost) {
                        continue;
                    }
                    const u64 room = capacity[level] - quotas[level];
                    if (room == 0u) {
                        continue;
                    }
                    const u64 extra = std::min(room, remaining / cost);
                    if (extra == 0u) {
                        continue;
                    }
                    quotas[level] += extra;
                    remaining -= extra * cost;
                    progress = true;
                }
            }
        };

        // Dirty/sky invalidation: clear the affected counts only, then spend at
        // most half of this frame's weighted ray budget repairing the region.
        // Published radiance/transmittance is never cleared, so samples outside
        // the repair share keep their last valid value until the ordinary
        // round-robin reaches them. Any unfinished dirty work stays represented
        // by count zero and is serviced by the reserved ordinary budget.
        encoder.pushDebugGroup("WorldLight.FieldTrace", rhi::MarkerColor{.r = 0.9f, .g = 0.6f, .b = 0.1f});
        PPR_DEFER {
            encoder.popDebugGroup();
        };

        u64 dirty_used = 0u;
        std::array<ProbeRect, kWorldLightMaxLevels> dirty_rects{};
        if (source.revision() != state.m_last_revision) {
            encoder.pushDebugGroup("WorldLight.FieldDirty", rhi::MarkerColor{.r = 0.9f, .g = 0.2f, .b = 0.9f});
            PPR_DEFER {
                encoder.popDebugGroup();
            };

            const WorldLightDirtyRect dirty = source.dirtyRect();
            const i32 sky_hour = source.skyHour();
            const float sky_intensity = state.m_plan.m_sky_intensity;
            const bool sky_changed = sky_hour != state.m_last_sky_hour or sky_intensity != state.m_last_sky_intensity;

            std::array<u32, kWorldLightMaxLevels> dirty_levels{};
            std::array<u64, kWorldLightMaxLevels> dirty_capacity{};
            u32 dirty_count = 0u;
            for (u32 level = 0u; level < level_count; ++level) {
                ProbeRect rect{};
                if (sky_changed and level + 1u == level_count) {
                    const WorldLightLevel &info = state.m_plan.m_levels[level];
                    rect = ProbeRect{0u, 0u, info.m_probes_x, info.m_probes_y};
                } else if (not dirty.empty()) {
                    const auto source_rect = probe_rect(state.m_plan.m_levels[level], dirty);
                    if (source_rect) {
                        rect = *source_rect;
                    }
                }
                if (rect.m_x1 <= rect.m_x0 or rect.m_y1 <= rect.m_y0) {
                    continue;
                }

                dirty_rects[level] = rect;
                dirty_capacity[level] = u64{rect.m_x1 - rect.m_x0} *
                                        (rect.m_y1 - rect.m_y0) *
                                        state.m_plan.m_levels[level].m_directions;
                dirty_levels[dirty_count++] = level;
            }

            std::array<u64, kWorldLightMaxLevels> dirty_quotas{};
            allocate_quotas(budget / 2u, dirty_levels, dirty_count, dirty_capacity, dirty_quotas);

            // Coarse-to-fine so each finer level can read the parent suffix this
            // frame already repaired from the current write buffer.
            for (u32 level = level_count; level-- > 0u;) {
                const ProbeRect rect = dirty_rects[level];
                if (rect.m_x1 <= rect.m_x0 or rect.m_y1 <= rect.m_y0) {
                    continue;
                }

                PPR_RETURN_ERROR_ON_FAIL(WorldLightField, dispatch_reset(level, rect));
                encoder.globalBarrier();

                const u64 quota = std::min(dirty_quotas[level], dirty_capacity[level]);
                PPR_RETURN_ERROR_ON_FAIL(WorldLightField, dispatch_probe_update(level, rect, quota));
                dirty_used += quota * levelCost_(level, level_count);
                encoder.globalBarrier();
            }

            state.m_last_revision = source.revision();
            state.m_last_sky_hour = sky_hour;
            state.m_last_sky_intensity = sky_intensity;
        }
        if (timing) {
            encoder.writeTimestamp(timing, 2u);
        }

        // Ordinary round-robin refresh uses everything the dirty phase did not
        // spend, so at least half of every non-zero budget preserves forward
        // progress even while revisions repeat. Levels are serviced
        // coarse-to-fine; each barrier makes the parent suffix available to the
        // next finer level from the current write buffer.
        const u64 ordinary_budget = budget - dirty_used;
        std::array<u32, kWorldLightMaxLevels> all_levels{};
        std::array<u64, kWorldLightMaxLevels> ordinary_capacity{};
        for (u32 level = 0u; level < level_count; ++level) {
            all_levels[level] = level;
            ordinary_capacity[level] = state.m_plan.m_levels[level].m_sample_count;
        }

        std::array<u64, kWorldLightMaxLevels> ordinary_quotas{};
        allocate_quotas(ordinary_budget, all_levels, level_count, ordinary_capacity, ordinary_quotas);
        for (u32 level = level_count; level-- > 0u;) {
            const WorldLightLevel &info = state.m_plan.m_levels[level];
            u64 remaining = ordinary_quotas[level];
            if (remaining == 0u) {
                continue;
            }

            const GroupDomain domain = groupDomain_(info, level);
            u32 phase = state.m_level_phase[level];
            u64 cursor = state.m_level_cursor[level];
            while (remaining != 0u) {
                const u64 take = std::min(remaining, domain.m_count - cursor);
                PPR_RETURN_ERROR_ON_FAIL(WorldLightField, dispatch_update(level, cursor, take, phase));
                remaining -= take;
                cursor += take;
                if (cursor >= domain.m_count) {
                    cursor = 0u;
                    phase = (phase + 1u) % 4u;
                    if (level == 0u and phase == 0u) {
                        ++state.m_cycle;
                    }
                }
            }
            state.m_level_cursor[level] = cursor;
            state.m_level_phase[level] = phase;
            encoder.globalBarrier();
        }
        encoder.globalBarrier();
        if (timing) {
            encoder.writeTimestamp(timing, 3u);
        }

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
            .m_cell_ids = state.m_source_cell_ids,
            .m_overlay = state.m_source_overlay,
            .m_materials = state.m_source_materials,
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

        // ParentVisibility traces the connector against the dense source and
        // incident atlas, so it needs the buffers the last update published and
        // at least two cascade levels. Irradiance now reads stored level-0 bins
        // only and no longer needs the source buffers.
        if (mode == EvaluateMode::kParentVisibility) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField, require_(
                                     state.m_plan.m_level_count >= 2u and
                                         state.m_source_cell_ids != nullptr and
                                         state.m_source_overlay != nullptr and
                                         state.m_source_materials != nullptr and
                                         state.m_source_atlas != nullptr,
                                     std::errc::not_connected));
        }

        state.m_evaluate_params.m_position_x = position.x;
        state.m_evaluate_params.m_position_y = position.y;
        state.m_evaluate_params.m_direction_x = direction.x;
        state.m_evaluate_params.m_direction_y = direction.y;
        state.m_evaluate_params.m_normal_x = normal.x;
        state.m_evaluate_params.m_normal_y = normal.y;
        state.m_evaluate_params.m_normal_z = normal.z;
        state.m_evaluate_params.m_roughness = roughness;
        state.m_evaluate_params.m_mode = static_cast<u32>(mode);

        // Rebind the current field constants, params and current radiance buffer
        // every call; frame/jitter and ping-pong state change between updates.
        {
            rhi::ShaderCursor cursor{state.m_evaluate_root.get()}, constants{};
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, cursor["g_world_light_field"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightField, constants.setData(&state.m_constants, sizeof(state.m_constants)));
        }
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

        // ParentVisibility's connector trace reads the same dense source and atlas
        // the update used. Bind them whenever the last update published them; the
        // other evaluate modes do not read them.
        const auto bind_source = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
            if (buffer == nullptr) {
                return {};
            }
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightField,
                rhi::ShaderCursor{state.m_evaluate_root.get()}[name].setBinding(rhi::Binding(buffer, range_(buffer))));
            return {};
        };
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_source("g_world_light_dense_ids", state.m_source_cell_ids));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_source("g_world_light_dense_overlay", state.m_source_overlay));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_source("g_world_light_materials", state.m_source_materials));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightField, bind_source("g_world_light_atlas", state.m_source_atlas));

        encoder.setBufferState(state.m_radiance[current].get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_evaluate_output.get(), rhi::ResourceState::UnorderedAccess);
        if (state.m_source_cell_ids != nullptr) {
            encoder.setBufferState(state.m_source_cell_ids, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(state.m_source_overlay, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(state.m_source_materials, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(state.m_source_atlas, rhi::ResourceState::ShaderResource);
        }

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
