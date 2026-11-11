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
        // acceleration storage and dispatch plumbing
        // ------------------------------------------------------------------

        rhi::BufferRange transportRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        std::error_code transportStorageBuffer(
            rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial,
            rhi::ComPtr<rhi::IBuffer> &result, const char *const label) {
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.elementSize = stride;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            desc.label = label;
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, device.createBuffer(desc, initial, result.writeRef()));
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
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, device.createTexture(desc, &initial, result.writeRef()));
            return {};
        }
    }

    // ------------------------------------------------------------------
    // acceleration resource ownership
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::ensureTransportResources_(rhi::IDevice &device) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, m_resources ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        auto &resources = *m_resources;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, resources.m_transport_programs[0u].m_program
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
                    WorldLightingPass,
                    transportStorageBuffer(
                        device, kVisibilityBufferBytes, static_cast<u32>(sizeof(lighting::VisibilityRecord)),
                        mask_poison.data(), attempt.m_visibility, "world-lighting visibility masks"));
            }

            if (not attempt.m_jump) {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass, transportJumpTexture(device, attempt.m_jump, "world-lighting jump table"));
            }

            for (u32 entry = 0u; entry < kTransportEntryCount; ++entry) {
                if (resources.m_transport_roots[entry][slot]) {
                    continue;
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    WorldLightingPass, device.createRootShaderObject(
                                           resources.m_transport_programs[entry].m_program,
                                           resources.m_transport_roots[entry][slot].writeRef()));
            }
        }

        return {};
    }

    // ------------------------------------------------------------------
    // full-domain acceleration encoding
    // ------------------------------------------------------------------

    std::error_code WorldLightingPass::encodeAcceleration_(const PreRenderContext &context) {
        auto &resources = *m_resources;
        auto &attempt = resources.m_status[resources.m_attempt_slot];
        rhi::ICommandEncoder &encoder = context.m_encoder;

        PPR_RETURN_ERROR_ON_FAIL(
            WorldLightingPass, attempt.m_visibility and attempt.m_jump and resources.m_resolved
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
                    WorldLightingPass, cursor[name].setBinding(rhi::Binding(buffer, transportRange(buffer))));
                return {};
            };

            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_resolved_cells", resources.m_resolved));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_visuals", attempt.m_inputs.m_visible_materials));
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, bind("g_visibility", attempt.m_visibility));
            if (entry == 1u) {
                PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, cursor["g_jump"].setBinding(rhi::Binding(attempt.m_jump)));
            }

            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldLightingPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
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
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, dispatch(0u, kWorldEdge / 32u, kWorldEdge / 32u));
        }

        // Visibility write -> jump read dependency. The jump dispatch reads at
        // most three masks per native cell and writes exactly one R8_UINT texel.
        encoder.globalBarrier();
        {
            encoder.pushDebugGroup("WorldLighting.Jump", {});
            PPR_DEFER {
                encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(WorldLightingPass, dispatch(1u, kWorldEdge / 16u, kWorldEdge / 16u));
        }

        // Jump write -> trace read dependency. Coarse/fine tracing binds both
        // allocations as SRVs; the next attempt's writer can only reuse this
        // slot after this submission and its readers completed.
        encoder.globalBarrier();
        encoder.setBufferState(attempt.m_visibility, rhi::ResourceState::ShaderResource);
        encoder.setTextureState(attempt.m_jump, rhi::ResourceState::ShaderResource);
        return {};
    }
}
