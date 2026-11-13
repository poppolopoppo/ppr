module;
#include "pP/Macros.h"

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
        // ------------------------------------------------------------------
        // cascade storage and dispatch plumbing
        // ------------------------------------------------------------------

        // Mirrors SourceFrame in assets/shaders/world_lighting_source.slang.
        struct alignas(16) SourceStageFrame {
            u32 m_edge{WorldLightingPass::kWorldEdge};
            u32 m_fine_edge{lighting::kFineCHEdge};
            u32 m_status_record{};
            u32 m_reserved{};
        };

        static_assert(sizeof(SourceStageFrame) == 16u and alignof(SourceStageFrame) == 16u);
        static_assert(PPR_OFFSETOF(SourceStageFrame, m_fine_edge) == 4u);
        static_assert(PPR_OFFSETOF(SourceStageFrame, m_status_record) == 8u);

        // Mirrors CascadeFrame in assets/shaders/world_lighting_cascade.slang.
        struct alignas(16) CascadeStageFrame {
            u32 m_level{};
            u32 m_frame_low{};
            u32 m_frame_high{};
            i32 m_sky_hour{-1};
            u32 m_status_record{};
            u32 m_reserved[3]{};
        };

        static_assert(sizeof(CascadeStageFrame) == 32u and alignof(CascadeStageFrame) == 16u);
        static_assert(PPR_OFFSETOF(CascadeStageFrame, m_frame_low) == 4u);
        static_assert(PPR_OFFSETOF(CascadeStageFrame, m_frame_high) == 8u);
        static_assert(PPR_OFFSETOF(CascadeStageFrame, m_sky_hour) == 12u);
        static_assert(PPR_OFFSETOF(CascadeStageFrame, m_status_record) == 16u);

        // Mirrors FineFrame in assets/shaders/world_lighting_fine.slang.
        struct alignas(16) FineStageFrame {
            u32 m_frame_low{};
            u32 m_frame_high{};
            u32 m_fine_edge{lighting::kFineCHEdge};
            u32 m_status_record{};
            float m_current_weight{};
            u32 m_first_field{};
            u32 m_reserved0{};
            u32 m_reserved1{};
        };

        static_assert(sizeof(FineStageFrame) == 32u and alignof(FineStageFrame) == 16u);
        static_assert(PPR_OFFSETOF(FineStageFrame, m_frame_high) == 4u);
        static_assert(PPR_OFFSETOF(FineStageFrame, m_fine_edge) == 8u);
        static_assert(PPR_OFFSETOF(FineStageFrame, m_status_record) == 12u);
        static_assert(PPR_OFFSETOF(FineStageFrame, m_current_weight) == 16u);
        static_assert(PPR_OFFSETOF(FineStageFrame, m_first_field) == 20u);

        rhi::BufferRange transportRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        std::error_code transportStorageBuffer(
            rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial,
            rhi::ComPtr<rhi::IBuffer> &result, const char *const label, const bool copy_destination = false) {
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.elementSize = stride;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            u32 usage = static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                        static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                        static_cast<u32>(rhi::BufferUsage::CopySource);
            if (copy_destination) {
                // Only the in-place fine field needs an upload seam so tests can
                // prime one record; other transport storage is never copied to.
                usage |= static_cast<u32>(rhi::BufferUsage::CopyDestination);
            }
            desc.usage = static_cast<rhi::BufferUsage>(usage);
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            desc.label = label;
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, device.createBuffer(desc, initial, result.writeRef()));
            return {};
        }

        std::error_code transportJumpTexture(
            rhi::IDevice &device, rhi::ComPtr<rhi::ITexture> &result, const char *const label) {
            const u32 edge = WorldLightingPass::kWorldEdge;
            std::vector<u8> poison(static_cast<std::size_t>(edge) * edge, 0xffu);
            const rhi::SubresourceData initial{poison.data(), static_cast<u64>(edge), static_cast<u64>(edge) * edge};

            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {edge, edge, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::R8Uint;
            desc.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                static_cast<u32>(rhi::TextureUsage::UnorderedAccess) |
                static_cast<u32>(rhi::TextureUsage::CopySource));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            desc.label = label;
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, device.createTexture(desc, &initial, result.writeRef()));
            return {};
        }
    }

    // ------------------------------------------------------------------
    // acceleration resource ownership
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::ensureTransportResources_(rhi::IDevice &device) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, m_resources ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        auto &resources = *m_resources;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, resources.m_transport_programs[0u].m_program
                               ? std::error_code{}
                               : std::make_error_code(std::errc::not_connected));

        // Initial contents are deliberately invalid: every record is rewritten
        // each frame, so a missed write can never masquerade as a valid empty
        // domain. Creation happens once per world, not per frame.
        for (u32 slot = 0u; slot < kStatusRingSize; ++slot) {
            auto &attempt = resources.m_status[slot];
            if (not attempt.m_visibility) {
                const std::vector<u8> mask_poison(kVisibilityBufferBytes, 0xffu);
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLighting,
                    transportStorageBuffer(
                        device, kVisibilityBufferBytes, static_cast<u32>(sizeof(lighting::VisibilityRecord)),
                        mask_poison.data(), attempt.m_visibility, "world-lighting visibility masks"));
            }

            if (not attempt.m_jump) {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLighting, transportJumpTexture(device, attempt.m_jump, "world-lighting jump table"));
            }

            for (u32 entry = 0u; entry < kTransportEntryCount; ++entry) {
                if (resources.m_transport_roots[entry][slot]) {
                    continue;
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLighting, device.createRootShaderObject(
                                       resources.m_transport_programs[entry].m_program,
                                       resources.m_transport_roots[entry][slot].writeRef()));
            }
        }

        return ensureCascadeResources_(device);
    }

    // ------------------------------------------------------------------
    // full-domain acceleration encoding
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::encodeAcceleration_(const PreRenderContext &context) {
        auto &resources = *m_resources;
        auto &attempt = resources.m_status[resources.m_attempt_slot];
        rhi::ICommandEncoder &encoder = context.m_encoder;

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, attempt.m_visibility and attempt.m_jump and resources.m_resolved
                               ? std::error_code{}
                               : std::make_error_code(std::errc::not_connected));

        // The projection wrote every resolved cell immediately before this
        // sequence; the transition orders those writes before the visibility
        // read.
        encoder.setBufferState(resources.m_resolved, rhi::ResourceState::ShaderResource);
        encoder.setBufferState(attempt.m_visibility, rhi::ResourceState::UnorderedAccess);
        encoder.setTextureState(attempt.m_jump, rhi::ResourceState::UnorderedAccess);

        const auto dispatch = [&](const u32 entry, const u32 groups_x, const u32 groups_y) -> std::error_code {
            auto &root = *resources.m_transport_roots[entry][resources.m_attempt_slot];
            rhi::ShaderCursor cursor{&root};

            const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLighting, cursor[name].setBinding(rhi::Binding(buffer, transportRange(buffer))));
                return {};
            };

            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_resolved_cells", resources.m_resolved));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_visuals", attempt.m_inputs.m_visible_materials));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_visibility", attempt.m_visibility));
            if (entry == 1u) {
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, cursor["g_jump"].setBinding(rhi::Binding(attempt.m_jump)));
            }

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLighting, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(resources.m_transport_programs[entry].m_pipeline, &root);
                pass->dispatchCompute(groups_x, groups_y, 1u);
            }

            return {};
        };

        // One 32x32-group dispatch writes all three levels; the first 4x4
        // threads own the tile-local reduction, so no inter-group ordering or
        // global clear exists.
        {
            encoder.pushDebugGroup("WorldLighting.Visibility", {});
            PPR_DEFER {
                encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, dispatch(0u, kWorldEdge / 32u, kWorldEdge / 32u));
        }

        // Visibility write -> jump read dependency. The jump dispatch reads at
        // most three masks per native cell and writes exactly one R8_UINT texel.
        encoder.globalBarrier();
        {
            encoder.pushDebugGroup("WorldLighting.Jump", {});
            PPR_DEFER {
                encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, dispatch(1u, kWorldEdge / 16u, kWorldEdge / 16u));
        }

        // Jump write -> trace read dependency. Coarse/fine tracing binds both
        // allocations as SRVs; the next attempt's writer can only reuse this
        // slot after this submission and its readers completed.
        encoder.globalBarrier();
        encoder.setBufferState(attempt.m_visibility, rhi::ResourceState::ShaderResource);
        encoder.setTextureState(attempt.m_jump, rhi::ResourceState::ShaderResource);
        return {};
    }

    // ------------------------------------------------------------------
    // cascade storage ownership
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::ensureCascadeResources_(rhi::IDevice &device) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, m_resources ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        auto &resources = *m_resources;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, resources.m_cascade_programs[0u].m_program and resources.m_fine_programs[0u].m_program
                               ? std::error_code{}
                               : std::make_error_code(std::errc::not_connected));

        // Creation-time zero contents are the approved cold start: an unvisited
        // interval has zero radiance and zero transmittance, never vacuum, and
        // source cells start black. None of these is reallocated per frame.
        if (not resources.m_cascades) {
            const std::vector<u8> zero(kCascadeBufferBytes, 0u);
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLighting, transportStorageBuffer(
                                   device, kCascadeBufferBytes, static_cast<u32>(sizeof(lighting::PackedInterval)),
                                   zero.data(), resources.m_cascades, "world-lighting coarse cascade intervals"));
        }

        if (not resources.m_source) {
            const std::vector<u8> zero(kSourceCellBytes, 0u);
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLighting, transportStorageBuffer(
                                   device, kSourceCellBytes, static_cast<u32>(sizeof(u32)), zero.data(),
                                   resources.m_source, "world-lighting cached source"));
        }

        if (not resources.m_fine_ch) {
            const std::vector<u8> zero(kFineCHBytes, 0u);
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLighting, transportStorageBuffer(
                                   device, kFineCHBytes, static_cast<u32>(sizeof(lighting::PackedCH)), zero.data(),
                                   resources.m_fine_ch, "world-lighting fine CH field", true));
        }

        for (u32 slot = 0u; slot < kStatusRingSize; ++slot) {
            for (u32 entry = 0u; entry < kCascadeEntryCount; ++entry) {
                if (resources.m_cascade_roots[entry][slot]) {
                    continue;
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLighting, device.createRootShaderObject(
                                       resources.m_cascade_programs[entry].m_program,
                                       resources.m_cascade_roots[entry][slot].writeRef()));
            }

            for (u32 entry = 0u; entry < kFineEntryCount; ++entry) {
                if (resources.m_fine_roots[entry][slot]) {
                    continue;
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLighting,
                    device.createRootShaderObject(
                        resources.m_fine_programs[entry].m_program, resources.m_fine_roots[entry][slot].writeRef()));
            }
        }

        return {};
    }

    // ------------------------------------------------------------------
    // cached source and continuous cascade encoding
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::encodeCascades_(
        const PreRenderContext &context, const WorldLightingFrame &frame) {
        auto &resources = *m_resources;
        auto &attempt = resources.m_status[resources.m_attempt_slot];
        const auto &inputs = attempt.m_inputs;
        rhi::ICommandEncoder &encoder = context.m_encoder;

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, resources.m_cascades and
                                   resources.m_source and
                                   resources.m_fine_ch and
                                   inputs.m_source_materials and
                                   inputs.m_thermal_lut
                               ? std::error_code{}
                               : std::make_error_code(std::errc::not_connected));

        // The source stage reads the field's pre-update DC. The owner's single
        // in-place field is the production prior; a known DC field may still be
        // supplied through the same optional publication slot for diagnostics.
        // Every source read is ordered before fine reconstruction writes the
        // field.
        rhi::IBuffer *const prior_ch = inputs.m_prior_ch ? inputs.m_prior_ch.get() : resources.m_fine_ch.get();
        if (inputs.m_prior_ch) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLighting, prior_ch->getDesc().size == kFineCHBytes and
                                       prior_ch->getDesc().elementSize == sizeof(lighting::PackedCH)
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::invalid_argument));
        }

        const u64 accepted = resources.m_accepted_attempts;
        const u32 frame_low = static_cast<u32>(accepted);
        const u32 frame_high = static_cast<u32>(accepted >> 32u);

        // Source preparation reads the pre-update field only. Fine
        // reconstruction runs after every dispatch below, so all source and
        // cascade dispatches observe the current materials plus the previous
        // field's angular mean.
        encoder.setBufferState(resources.m_source, rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(attempt.m_status, rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(inputs.m_source_materials, rhi::ResourceState::ShaderResource);
        encoder.setBufferState(inputs.m_thermal_lut, rhi::ResourceState::ShaderResource);
        encoder.setBufferState(prior_ch, rhi::ResourceState::ShaderResource);
        // The projection wrote the particle row rank and the Q16 fill word
        // earlier in this submission. These transitions order those atomics
        // before the source stage's read of them.
        encoder.setBufferState(resources.m_scratch[1u], rhi::ResourceState::ShaderResource);
        encoder.setBufferState(resources.m_scratch[2u], rhi::ResourceState::ShaderResource);

        const auto bind = [&](const u32 entry, const u32 level) -> std::error_code {
            auto &root = *resources.m_cascade_roots[entry][resources.m_attempt_slot];
            rhi::ShaderCursor cursor{&root};

            const auto bind_buffer = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLighting, cursor[name].setBinding(rhi::Binding(buffer, transportRange(buffer))));
                return {};
            };

            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_resolved_cells", resources.m_resolved));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_visuals", inputs.m_visible_materials));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_cell_source", resources.m_source));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_numeric_status", attempt.m_status));

            rhi::ShaderCursor constants;
            if (entry == 0u) {
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_source_materials", inputs.m_source_materials));
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_thermal_lut", inputs.m_thermal_lut));
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_prior_ch", prior_ch));
                // Fill-weighted transport source: the cell's own winner rank and
                // the Q16 fill the projection accumulated for it. Both are
                // read-only here and neither changes the winner selection.
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_cell_fill", resources.m_scratch[2u]));
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_best_row", resources.m_scratch[1u]));
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, cursor["g_source_frame"].getDereferenced(constants));

                const SourceStageFrame values{.m_status_record = kNumericStageSource};
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, constants.setData(&values, sizeof(values)));
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind_buffer("g_cascades", resources.m_cascades));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, cursor["g_jump"].setBinding(rhi::Binding(attempt.m_jump)));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, cursor["g_cascade"].getDereferenced(constants));

            CascadeStageFrame values{};
            values.m_level = level;
            values.m_frame_low = frame_low;
            values.m_frame_high = frame_high;
            values.m_sky_hour = frame.m_sky_hour;
            values.m_status_record = entry == 1u ? kNumericStageTerminal : kNumericStageCascade;
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, constants.setData(&values, sizeof(values)));
            return {};
        };

        const auto dispatch = [&](const u32 entry, const u32 groups) -> std::error_code {
            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLighting, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(
                    resources.m_cascade_programs[entry].m_pipeline,
                    resources.m_cascade_roots[entry][resources.m_attempt_slot].get());
                pass->dispatchCompute(groups, groups, 1u);
            }

            return {};
        };

        const u32 source_groups = kWorldEdge / lighting::kCascadeDispatchThreads;
        const u32 cascade_groups = lighting::kCascadeDomainEdge / lighting::kCascadeDispatchThreads;

        {
            const hal::ProfileScope scope{"App.Pass.WorldLighting.Source"};
            encoder.pushDebugGroup("WorldLighting.Source", {});
            PPR_DEFER {
                encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind(0u, 0u));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, dispatch(0u, source_groups));
        }

        // Source write -> trace read dependency before any cascade dispatch.
        encoder.globalBarrier();
        encoder.setBufferState(resources.m_source, rhi::ResourceState::ShaderResource);
        encoder.setBufferState(resources.m_cascades, rhi::ResourceState::UnorderedAccess);

        // The terminal dispatch writes the coarsest retained cascade level; it
        // is the L5 pass of the same contract range set.
        {
            const hal::ProfileScope scope{"WorldLighting.Cascade.L5"};
            encoder.pushDebugGroup("WorldLighting.Cascade.L5", {});
            PPR_DEFER {
                encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind(1u, 0u));
            PPR_RETURN_ERROR_ON_FAIL(WorldLighting, dispatch(1u, cascade_groups));
        }

        // Terminal write -> L4 parent read dependency, then one dependency per
        // level. Each level reads the already-merged parent intervals and writes
        // only its own probe/atlas records.
        constexpr std::array<const char *, kCascadeLevels> kCascadeMarkerNames{
            "WorldLighting.Cascade.L1",
            "WorldLighting.Cascade.L2",
            "WorldLighting.Cascade.L3",
            "WorldLighting.Cascade.L4",
            "WorldLighting.Cascade.L5",
        };
        for (u32 level = kCascadeLevels - 1u; level >= 1u; --level) {
            encoder.globalBarrier();
            {
                const hal::ProfileScope scope{kCascadeMarkerNames[level - 1u]};
                encoder.pushDebugGroup(kCascadeMarkerNames[level - 1u], {});
                PPR_DEFER {
                    encoder.popDebugGroup();
                };
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind(2u, level));
                PPR_RETURN_ERROR_ON_FAIL(WorldLighting, dispatch(2u, cascade_groups));
            }
        }

        // Cascade write -> fine reconstruction / scene read dependency.
        encoder.globalBarrier();
        encoder.setBufferState(resources.m_cascades, rhi::ResourceState::ShaderResource);
        return {};
    }

    // ------------------------------------------------------------------
    // fine CH reconstruction and in-place accumulation
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::encodeFine_(const PreRenderContext &context) {
        auto &resources = *m_resources;
        auto &attempt = resources.m_status[resources.m_attempt_slot];
        rhi::ICommandEncoder &encoder = context.m_encoder;

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, resources.m_fine_programs[0u].m_program and
                                   resources.m_fine_roots[0u][resources.m_attempt_slot] and
                                   resources.m_fine_ch and
                                   resources.m_cascades and
                                   resources.m_source and
                                   attempt.m_jump
                               ? std::error_code{}
                               : std::make_error_code(std::errc::not_connected));

        const u64 accepted = resources.m_accepted_attempts;
        const bool first_field = accepted == 0u;
        const TimeSpan delta = attempt.m_active_elapsed - resources.m_accepted_active_elapsed;
        const double delta_seconds = time::seconds(delta);

        FineStageFrame values{};
        values.m_frame_low = static_cast<u32>(accepted);
        values.m_frame_high = static_cast<u32>(accepted >> 32u);
        values.m_status_record = kNumericStageFine;
        values.m_current_weight = first_field ? 0.0f : lighting::fineAccumulationWeight(delta_seconds);
        values.m_first_field = first_field ? 1u : 0u;

        const hal::ProfileScope scope{"WorldLighting.FineCH"};
        encoder.pushDebugGroup("WorldLighting.FineCH", {});
        PPR_DEFER {
            encoder.popDebugGroup();
        };

        // Every source-preparation read of the prior CH DC has already been
        // ordered before this barrier. The field is read and written only at
        // the invocation's own record; there is no neighbour read or history
        // copy.
        encoder.globalBarrier();
        encoder.setBufferState(resources.m_fine_ch, rhi::ResourceState::UnorderedAccess);

        auto &root = *resources.m_fine_roots[0u][resources.m_attempt_slot];
        rhi::ShaderCursor cursor{&root};
        const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLighting, cursor[name].setBinding(rhi::Binding(buffer, transportRange(buffer))));
            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_resolved_cells", resources.m_resolved));
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_visuals", attempt.m_inputs.m_visible_materials));
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_cell_source", resources.m_source));
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_cascades", resources.m_cascades));
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_fine_ch", resources.m_fine_ch));
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, bind("g_numeric_status", attempt.m_status));
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, cursor["g_jump"].setBinding(rhi::Binding(attempt.m_jump)));

        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, cursor["g_fine"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(WorldLighting, constants.setData(&values, sizeof(values)));

        auto *const pass = encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLighting, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        {
            PPR_DEFER {
                pass->end();
            };
            pass->bindPipeline(resources.m_fine_programs[0u].m_pipeline, &root);
            pass->dispatchCompute(kFineCHEdge / kFineDispatchThreads, kFineCHEdge / kFineDispatchThreads, 1u);
        }

        // Fine write -> scene read dependency. The field stays an SRV for the
        // scene/visible readers in this submission and for the next submitted
        // frame's source preparation on the same ordered queue.
        encoder.globalBarrier();
        encoder.setBufferState(resources.m_fine_ch, rhi::ResourceState::ShaderResource);
        return {};
    }
}
