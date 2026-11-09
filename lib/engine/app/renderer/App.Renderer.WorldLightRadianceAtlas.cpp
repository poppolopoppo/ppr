module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;

import :renderer.world_light_field;
import :renderer.world_light_radiance_atlas;
import :renderer.world_light_source_gpu;
import :renderer.world_light_types;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

namespace pP::lighting {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(WorldLightRadianceAtlas, info, none)

    // ------------------------------------------------------------------
    // per-cell radiance atlas
    // ------------------------------------------------------------------

    namespace {
        inline constexpr u32 kNumthreads = 64u;
        // encode() has no injected frame delta: one recorded pass represents one
        // 60 Hz frame, so the EMA's exp(-k * dt) uses this fixed step and the
        // retention setting keeps its per-60-Hz meaning.
        inline constexpr float kFrameDeltaSeconds = 1.0f / 60.0f;

        // Matched field-for-field by the Slang WorldLightRadianceAtlasConstants.
        struct WorldLightRadianceAtlasConstants {
            u32 m_width{}, m_height{};
            float m_temporal_rate{}, m_frame_delta{};
        };
        static_assert(
            sizeof(WorldLightRadianceAtlasConstants) == 16u and alignof(WorldLightRadianceAtlasConstants) == 4u);

        std::error_code require_(const bool valid, const std::errc error = std::errc::invalid_argument) noexcept {
            return valid ? std::error_code{} : std::make_error_code(error);
        }

        rhi::BufferRange range_(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }
    }

    struct WorldLightRadianceAtlas::State {
        u32 m_width{};
        u32 m_height{};
        float m_temporal_retention{0.9f};

        rhi::IDevice *m_device{};
        IShaderService *m_shaders{};

        shader::SharedModule m_module;
        rhi::ComPtr<rhi::IShaderProgram> m_program;
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline;
        rhi::ComPtr<rhi::IShaderObject> m_root;
        rhi::ComPtr<rhi::IBuffer> m_cells;

        WorldLightRadianceAtlasConstants m_constants{};

        bool m_ready{};
    };

    WorldLightRadianceAtlas::WorldLightRadianceAtlas() : m_state{std::make_unique<State>()} {
        setTemporalRetention(m_state->m_temporal_retention);
    }

    WorldLightRadianceAtlas::~WorldLightRadianceAtlas() {
        std::ignore = shutdown();
    }

    std::error_code WorldLightRadianceAtlas::initialize(
        IRhiService &rhi, IShaderService &shaders, const WorldLightPlan &plan,
        const std::filesystem::path &content_dir) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, require_(not state.m_ready, std::errc::already_connected));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, require_(plan.m_width != 0u and plan.m_height != 0u));

        const u64 cell_count = u64{plan.m_width} * plan.m_height;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, require_(cell_count <= std::numeric_limits<u32>::max()));

        state.m_device = &rhi.getDevice();
        state.m_shaders = &shaders;
        state.m_width = plan.m_width;
        state.m_height = plan.m_height;
        state.m_constants.m_width = plan.m_width;
        state.m_constants.m_height = plan.m_height;
        state.m_constants.m_frame_delta = kFrameDeltaSeconds;

        // Load the atlas module and build its single compute program/pipeline.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightRadianceAtlas, shaders.loadModuleFromFile(
                                         content_dir / TEXT("shaders") / TEXT("world_light_radiance_atlas.slang"),
                                         "world_light_radiance_atlas", state.m_module.writeRef()));

        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightRadianceAtlas,
            state.m_module->findEntryPointByName("updateWorldLightRadianceAtlas", entry.writeRef()));

        slang::IComponentType *entries[]{entry.get()};
        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = state.m_module.get();
        description.slangEntryPoints = entries;
        description.slangEntryPointCount = 1u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightRadianceAtlas,
            state.m_device->createShaderProgram(description, state.m_program.writeRef(), diagnostics.writeRef()));

        rhi::ComputePipelineDesc pipeline_desc{};
        pipeline_desc.program = state.m_program;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightRadianceAtlas, state.m_device->createComputePipeline(pipeline_desc, state.m_pipeline.writeRef()));

        // One packed u32 per native cell, created zeroed so a fresh atlas is
        // exact zero before the first encode.
        const u64 atlas_bytes = cell_count * sizeof(u32);
        const std::vector<u32> zero_cells(cell_count, 0u);

        rhi::BufferDesc buffer_desc{};
        buffer_desc.size = atlas_bytes;
        buffer_desc.elementSize = sizeof(u32);
        buffer_desc.memoryType = rhi::MemoryType::DeviceLocal;
        buffer_desc.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::ShaderResource) |
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
            static_cast<u32>(rhi::BufferUsage::CopySource) |
            static_cast<u32>(rhi::BufferUsage::CopyDestination));
        buffer_desc.defaultState = rhi::ResourceState::UnorderedAccess;
        buffer_desc.label = "world light radiance atlas";
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightRadianceAtlas,
            state.m_device->createBuffer(buffer_desc, zero_cells.data(), state.m_cells.writeRef()));

        // Bindings (borrowed source buffers and the atlas) are set per encode.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightRadianceAtlas,
            state.m_device->createRootShaderObject(state.m_program.get(), state.m_root.writeRef()));

        state.m_ready = true;
        PPR_LOG(
            WorldLightRadianceAtlas, info, "WorldLightRadianceAtlas initialized",
            {{"width", plan.m_width}, {"height", plan.m_height}, {"atlas_bytes", atlas_bytes}});
        return default_value_v;
    }

    std::error_code WorldLightRadianceAtlas::encode(
        rhi::ICommandEncoder &encoder, const WorldLightSourceGpu &source, const WorldLightFieldView &field) {
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, require_(state.m_ready, std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightRadianceAtlas, require_(field.m_radiance != nullptr and field.m_constants != nullptr));

        encoder.pushDebugGroup("WorldLight.RadianceAtlas", rhi::MarkerColor{.r = 0.9f, .g = 0.5f, .b = 0.9f});
        PPR_DEFER {
            encoder.popDebugGroup();
        };

        {
            rhi::ShaderCursor cursor{state.m_root.get()}, constants{}, field_constants{};
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightRadianceAtlas, cursor["g_world_light_atlas_constants"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightRadianceAtlas, constants.setData(&state.m_constants, sizeof(state.m_constants)));

            // The gather maps cell centres through the field's own origin and
            // cell size, so the atlas and the field must share one snapshot.
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightRadianceAtlas, cursor["g_world_light_field"].getDereferenced(field_constants));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightRadianceAtlas, field_constants.setData(field.m_constants, sizeof(WorldLightFieldConstants)));
        }

        const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightRadianceAtlas,
                rhi::ShaderCursor{state.m_root.get()}[name].setBinding(rhi::Binding(buffer, range_(buffer))));
            return {};
        };
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, bind("g_world_light_ids", source.cellIds()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, bind("g_world_light_overlay", source.overlay()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, bind("g_world_light_atlas", state.m_cells.get()));
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, bind("g_world_light_radiance_prev", field.m_radiance));

        // The dense source ids/overlay are read through the same RW bindings the
        // field uses; the merged previous field is only read, so it moves to a
        // shader-resource state before the gather (the field update later takes
        // it back for the ping-pong copy).
        encoder.setBufferState(source.cellIds(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(source.overlay(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(state.m_cells.get(), rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(field.m_radiance, rhi::ResourceState::ShaderResource);

        const u32 cell_count = state.m_width * state.m_height;
        const u32 groups = (cell_count + kNumthreads - 1u) / kNumthreads;

        auto *const pass = encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(WorldLightRadianceAtlas, require_(pass != nullptr, std::errc::io_error));
        pass->bindPipeline(state.m_pipeline.get(), state.m_root.get());
        pass->dispatchCompute(groups, 1u, 1u);
        pass->end();
        return default_value_v;
    }

    rhi::IBuffer *WorldLightRadianceAtlas::cells() const noexcept {
        return m_state->m_cells.get();
    }

    void WorldLightRadianceAtlas::setTemporalRetention(const float retention) noexcept {
        State &state = *m_state;
        state.m_temporal_retention = std::clamp(retention, 0.0f, 1.0f);

        // exp(-k * 1/60) == retention, so k = -60 * ln(retention). retention 1
        // freezes history (k = 0); retention 0 snaps to the new sample (k is
        // capped by the small positive floor instead of reaching infinity).
        state.m_constants.m_temporal_rate = state.m_temporal_retention >= 1.0f
                                                ? 0.0f
                                                : -std::log(std::max(state.m_temporal_retention, 1.0e-6f)) /
                                                      kFrameDeltaSeconds;
    }

    std::error_code WorldLightRadianceAtlas::shutdown() {
        State &state = *m_state;

        state.m_root.setNull();
        state.m_pipeline.setNull();
        state.m_program.setNull();
        state.m_cells.setNull();
        state.m_module = {};

        state.m_ready = false;
        state.m_device = nullptr;
        state.m_shaders = nullptr;
        state.m_width = 0u;
        state.m_height = 0u;
        return default_value_v;
    }
} // namespace pP::lighting
