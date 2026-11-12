module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;

import :renderer.frame_fence;
import :renderer.types;
import :renderer.world_lighting;
import :renderer.world_lighting_pass;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    namespace {
        // Mirrors ProjectionFrame in assets/shaders/world_lighting_projection.slang.
        struct alignas(16) ProjectionFrame {
            float m_origin{};
            float m_edge{static_cast<float>(WorldLightingPass::kWorldEdge)};
            u32 m_count{};
            u32 m_moving_count{};
            u32 m_material_count{};
            u32 m_status_record{};
            u32 m_reserved[2]{};
        };

        static_assert(sizeof(ProjectionFrame) == 32u and alignof(ProjectionFrame) == 16u);
        static_assert(PPR_OFFSETOF(ProjectionFrame, m_edge) == 4u);
        static_assert(PPR_OFFSETOF(ProjectionFrame, m_count) == 8u);
        static_assert(PPR_OFFSETOF(ProjectionFrame, m_material_count) == 16u);
        static_assert(PPR_OFFSETOF(ProjectionFrame, m_status_record) == 20u);

        rhi::BufferRange projectionBufferRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }
    }

    // ------------------------------------------------------------------
    // projection encoding
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::encodeProjection_(const PreRenderContext &context) {
        auto &resources = *m_resources;
        auto &attempt = resources.m_status[resources.m_attempt_slot];
        const auto &inputs = attempt.m_inputs;
        const auto &particles = inputs.m_particles;
        const bool published = particles.current();
        const u32 count = published ? particles.m_metadata->m_count : 0u;

        ProjectionFrame values{};
        values.m_origin = published ? static_cast<float>(particles.m_origin) : 0.0f;
        values.m_count = count;
        values.m_moving_count = published ? particles.m_metadata->m_moving_count : 0u;
        values.m_material_count = inputs.m_material_count;

        rhi::ICommandEncoder &encoder = context.m_encoder;
        const hal::ProfileScope scope{"WorldLighting.ResolveCells"};
        encoder.pushDebugGroup("WorldLighting.ResolveCells", {});
        PPR_DEFER {
            encoder.popDebugGroup();
        };

        // Immutable inputs and projection outputs keep one state for the whole
        // ordered sequence; each reduction is separated by a UAV barrier.
        encoder.setBufferState(inputs.m_terrain, rhi::ResourceState::ShaderResource);
        encoder.setBufferState(inputs.m_visible_materials, rhi::ResourceState::ShaderResource);
        if (published) {
            encoder.setBufferState(particles.m_numerical, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(particles.m_metadata->m_rows, rhi::ResourceState::ShaderResource);
        }

        for (auto *const buffer:
            {resources.m_scratch[0u].get(), resources.m_scratch[1u].get(), resources.m_scratch[2u].get(),
                resources.m_resolved.get(), attempt.m_status.get()}) {
            encoder.setBufferState(buffer, rhi::ResourceState::UnorderedAccess);
        }

        const auto dispatch = [&](const u32 entry, const u32 groups) -> std::error_code {
            auto &root = *resources.m_roots[entry][resources.m_attempt_slot];
            rhi::ShaderCursor cursor{&root};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, cursor["g_projection"].getDereferenced(constants));

            ProjectionFrame frame = values;
            frame.m_status_record = entry;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, constants.setData(&frame, sizeof(frame)));

            const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass, cursor[name].setBinding(rhi::Binding(buffer, projectionBufferRange(buffer))));
                return {};
            };

            if (entry == 0u) {
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_best_key", resources.m_scratch[0u]));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_best_row", resources.m_scratch[1u]));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_cell_fill", resources.m_scratch[2u]));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_numeric_status", attempt.m_status));
            } else if (entry == 3u) {
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_terrain", inputs.m_terrain));
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass, bind(
                                           "g_metadata", published ? particles.m_metadata->m_rows.get()
                                                                   : resources.m_empty_metadata.get()));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_best_row", resources.m_scratch[1u]));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_resolved", resources.m_resolved));
            } else {
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_metadata", particles.m_metadata->m_rows));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_numerical", particles.m_numerical));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_visuals", inputs.m_visible_materials));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_terrain", inputs.m_terrain));
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_best_key", resources.m_scratch[0u]));
                if (entry == 1u) {
                    // The key reduction owns the per-cell fill accumulation; the
                    // row reduction only refines the winning row.
                    PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_cell_fill", resources.m_scratch[2u]));
                    PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_numeric_status", attempt.m_status));
                } else {
                    PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_best_row", resources.m_scratch[1u]));
                }
            }

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(resources.m_programs[entry].m_pipeline, &root);
                pass->dispatchCompute(groups, 1u, 1u);
            }

            encoder.globalBarrier();
            return {};
        };

        // Clear every scratch grid (ordered key, row rank and Q16 fill) and all
        // four status records, then reduce the same immutable committed input
        // twice and overwrite every resolved cell. The resolve runs even for a
        // zero-particle world.
        constexpr u32 cell_groups = WorldLightingPass::kCellCount / 256u;
        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, dispatch(0u, cell_groups));
        if (count != 0u) {
            const u32 particle_groups = (count + 255u) / 256u;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, dispatch(1u, particle_groups));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, dispatch(2u, particle_groups));
        }

        PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, dispatch(3u, cell_groups));
        return {};
    }
}
