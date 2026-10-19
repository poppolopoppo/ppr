module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.retro_pass;
import :renderer.frame_fence;
import :renderer.types;
import std;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(RetroPass, debug, none)

    // ------------------------------------------------------------------
    // palette and constant-buffer ABI
    // ------------------------------------------------------------------

    namespace {
        // DawnBringer-32 sRGB hex table. The pass stores normalized sRGB
        // floats; the shader quantizes in sRGB and converts back to linear.
        constexpr std::array<u32, RetroPass::kPaletteSize> kPaletteRgb{
            0x000000u,
            0x222034u,
            0x45283cu,
            0x663931u,
            0x8f563bu,
            0xdf7126u,
            0xd9a066u,
            0xeec39au,
            0xfbf236u,
            0x99e550u,
            0x6abe30u,
            0x37946eu,
            0x4b692fu,
            0x524b24u,
            0x323c39u,
            0x3f3f74u,
            0x306082u,
            0x5b6ee1u,
            0x639bffu,
            0x5fcde4u,
            0xcbdbfcu,
            0xffffffu,
            0x9badb7u,
            0x847e87u,
            0x696a6au,
            0x595652u,
            0x76428au,
            0xac3232u,
            0xd95763u,
            0xd77bbau,
            0x8f974au,
            0x8a6f30u,
        };

        constexpr float kDitherStrength{0.0625f};

        // CPU mirror of the shader's ConstantBuffer<RetroParameters>. Plain
        // float arrays keep the shader's float4/float2 component layout while
        // avoiding Mango's explicit vector default constructors.
        struct alignas(16) RetroParameters {
            float m_palette[32][4];
            float m_source_extent[2];
            float m_dither_strength;
            u32 m_reserved;
        };
        static_assert(sizeof(RetroParameters) == 528u);
        static_assert(alignof(RetroParameters) == 16u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_source_extent) == 512u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_dither_strength) == 520u);
    }

    namespace fs = std::filesystem;

    // ------------------------------------------------------------------
    // scene target geometry and signature
    // ------------------------------------------------------------------

    u32 RetroPass::clampPixelScale(const u32 scale) noexcept {
        return std::clamp(scale, kMinPixelScale, kMaxPixelScale);
    }

    int2 RetroPass::sceneExtent(const int2 native_extent, const u32 pixel_scale) noexcept {
        const i32 scale = static_cast<i32>(clampPixelScale(pixel_scale));
        // Non-positive native extents collapse to one pixel, and both axes are
        // divided independently so one degenerate axis never distorts the other.
        const auto axis_extent = [scale](
                                     const i32 native) { return native > 0 ? std::max<i32>(1, native / scale) : 1; };
        return int2{axis_extent(native_extent.x), axis_extent(native_extent.y)};
    }

    RenderPipelineSignature RetroPass::scenePipelineSignature() noexcept {
        RenderPipelineSignature signature{};
        signature.m_color_formats[0] = rhi::Format::RGBA16Float;
        signature.m_color_format_count = 1u;
        signature.m_depth_stencil_format = rhi::Format::D32Float;
        signature.m_sample_count = SampleCount::x1;
        return signature;
    }

    // ------------------------------------------------------------------
    // initialization and teardown
    // ------------------------------------------------------------------

    std::error_code RetroPass::initialize(
        IRhiService &rhi_service, IShaderService &shader_service, const fs::path &content_dir) {
        rhi::IDevice &device = rhi_service.getDevice();

        bool initialization_complete = false;
        PPR_DEFER {
            if (not initialization_complete) {
                // The same idempotent, wait-free release the teardown paths
                // use, so a partial program/sampler acquisition never leaks.
                releaseGpuResources_();
            }
        };

        PPR_RETURN_ERROR_ON_FAIL(RetroPass, createShaderProgram_(shader_service, device, content_dir));

        // Point sampling with edge clamping: the source is an exact texel grid
        // and the fullscreen triangle must never bleed across its border.
        rhi::SamplerDesc sampler_desc{};
        sampler_desc.minFilter = rhi::TextureFilteringMode::Point;
        sampler_desc.magFilter = rhi::TextureFilteringMode::Point;
        sampler_desc.mipFilter = rhi::TextureFilteringMode::Point;
        sampler_desc.addressU = rhi::TextureAddressingMode::ClampToEdge;
        sampler_desc.addressV = rhi::TextureAddressingMode::ClampToEdge;
        sampler_desc.addressW = rhi::TextureAddressingMode::ClampToEdge;
        sampler_desc.maxAnisotropy = 1u;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createSampler(sampler_desc, m_sampler.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, m_sampler->getDescriptorHandle(&m_sampler_handle));

        for (u32 index = 0u; index < kPaletteSize; ++index) {
            const u32 rgb = kPaletteRgb[index];
            m_palette[index] = float4{
                static_cast<float>((rgb >> 16u) & 0xffu) / 255.0f, static_cast<float>((rgb >> 8u) & 0xffu) / 255.0f,
                static_cast<float>(rgb & 0xffu) / 255.0f, 1.0f};
        }

        m_ready = true;
        initialization_complete = true;

        PPR_LOG(RetroPass, info, "RetroPass initialized");
        return default_value_v;
    }

    std::error_code RetroPass::createShaderProgram_(
        IShaderService &shader_service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule retro_shader{};
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            shader_service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("retro_present.slang"), "retro_present", retro_shader.writeRef()));

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, retro_shader->findEntryPointByName("vertexRetroPresent", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, retro_shader->findEntryPointByName("fragmentRetroPresent", fragment_ep.writeRef()));

        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = retro_shader.get();
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, device.createShaderProgram(program_desc, m_program.writeRef(), diagnostics.writeRef()));

        for (auto &root: m_scene_roots) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createRootShaderObject(m_program.get(), root.writeRef()));
        }

        return default_value_v;
    }

    std::error_code RetroPass::shutdown() {
        PPR_LOG(RetroPass, info, "RetroPass shut down");
        releaseGpuResources_();
        return default_value_v;
    }

    std::error_code RetroPass::notifyDeviceLost() noexcept {
        // The GPU objects are gone, so nothing is worth waiting for: drain the
        // deferred releases and drop every owned resource outright.
        releaseGpuResources_();
        return default_value_v;
    }

    void RetroPass::releaseGpuResources_() noexcept {
        std::ignore = m_deferred_releases.drainAll();
        m_last_submitted_value = kFenceValueNone;

        m_pipeline.setNull();
        m_pipeline_key.reset();
        m_scene_roots = {};
        m_sampler.setNull();
        m_sampler_handle = {};
        m_program.setNull();

        m_scene_slots = {};
        m_scene_cursor = 0u;
        m_scene_slot = 0u;
        m_scene_ready = false;
        m_scene_recorded = false;
        m_ready = false;
    }

    // ------------------------------------------------------------------
    // offscreen scene preparation
    // ------------------------------------------------------------------

    std::error_code RetroPass::createSceneTarget_(rhi::IDevice &device, SceneTarget &slot, const int2 extent) {
        const u32 width = safe_narrowing(extent.x);
        const u32 height = safe_narrowing(extent.y);

        rhi::TextureDesc color_desc{};
        color_desc.type = rhi::TextureType::Texture2D;
        color_desc.size = {.width = width, .height = height, .depth = 1u};
        color_desc.arrayLength = 1u;
        color_desc.mipCount = 1u;
        color_desc.format = rhi::Format::RGBA16Float;
        color_desc.memoryType = rhi::MemoryType::DeviceLocal;
        color_desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        color_desc.defaultState = rhi::ResourceState::ShaderResource;
        color_desc.label = "retro scene color";

        rhi::ComPtr<rhi::ITexture> color;
        rhi::ComPtr<rhi::ITextureView> color_view;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(color_desc, nullptr, color.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, color->getDefaultView(color_view.writeRef()));

        rhi::TextureDesc depth_desc{};
        depth_desc.type = rhi::TextureType::Texture2D;
        depth_desc.size = {.width = width, .height = height, .depth = 1u};
        depth_desc.arrayLength = 1u;
        depth_desc.mipCount = 1u;
        depth_desc.format = rhi::Format::D32Float;
        depth_desc.memoryType = rhi::MemoryType::DeviceLocal;
        depth_desc.usage = rhi::TextureUsage::DepthStencil;
        depth_desc.defaultState = rhi::ResourceState::DepthWrite;
        depth_desc.label = "retro scene depth";

        rhi::ComPtr<rhi::ITexture> depth;
        rhi::ComPtr<rhi::ITextureView> depth_view;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(depth_desc, nullptr, depth.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, depth->getDefaultView(depth_view.writeRef()));

        slot.m_color = std::move(color);
        slot.m_color_view = std::move(color_view);
        slot.m_depth = std::move(depth);
        slot.m_depth_view = std::move(depth_view);
        slot.m_extent = extent;

        return default_value_v;
    }

    std::error_code RetroPass::prepareScene(
        const PreRenderContext &context, const RenderPipelineSignature &scene_signature, const int2 extent,
        const DrawCallback encode_scene) {
        const hal::ProfileScope scope{"App.Pass.Retro.Scene"};

        m_scene_ready = false;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            extent.x > 0 and extent.y > 0 ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        std::array<FenceCompletionValue, kFrameRingSlots> values;
        for (u32 slot = 0u; slot < kFrameRingSlots; ++slot) {
            values[slot] = m_scene_slots[slot].m_last_use;
        }

        const u32 selected = selectRingSlot(context.m_completed_fence_value, values, m_scene_cursor);
        FenceCompletionValue effective_completed = context.m_completed_fence_value;
        if (not slotFenceComplete(context.m_completed_fence_value, values[selected])) {
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, context.m_wait_for_fence ? context.m_wait_for_fence(values[selected])
                                                    : std::make_error_code(std::errc::operation_would_block));
            // A successful value-scoped wait proves completion even when the
            // borrowed frame context still carries its earlier snapshot.
            effective_completed = std::max(effective_completed, values[selected]);
        }
        m_scene_cursor = selected;

        auto &slot = m_scene_slots[selected];
        // The selected slot's previous use is complete at this point (waited
        // out above when stale), so replacing its targets is always safe.
        PPR_ASSERT(slotFenceComplete(effective_completed, slot.m_last_use));
        if (not slot.m_color or slot.m_extent.x != extent.x or slot.m_extent.y != extent.y) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, createSceneTarget_(context.m_device, slot, extent));
        }

        const rhi::SubresourceRange target_range{
            .layer = 0u,
            .layerCount = 1u,
            .mip = 0u,
            .mipCount = 1u,
        };
        context.m_encoder.setTextureState(slot.m_color, target_range, rhi::ResourceState::RenderTarget);
        context.m_encoder.setTextureState(slot.m_depth, target_range, rhi::ResourceState::DepthWrite);

        const rhi::RenderPassColorAttachment color_attachment{
            .view = slot.m_color_view,
            .clearValue = {0.1f, 0.1f, 0.2f, 1.0f},
        };
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = slot.m_depth_view;
        depth_attachment.depthClearValue = 1.0f;
        rhi::RenderPassDesc pass_desc{};
        pass_desc.colorAttachments = &color_attachment;
        pass_desc.colorAttachmentCount = 1u;
        pass_desc.depthStencilAttachment = &depth_attachment;
        rhi::IRenderPassEncoder *const pass = context.m_encoder.beginRenderPass(pass_desc);
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));

        const RenderPipelineKey scene_key(scene_signature);
        // The scene content draws through this pass's own fence hook: forward
        // to the frame context's hook while it is bound, and stay empty when
        // it is not (the content passes treat an empty hook as a block).
        FenceWaitHook scene_wait{};
        if (context.m_wait_for_fence) {
            scene_wait = FenceWaitHook{[&context](const FenceCompletionValue value) -> std::error_code {
                return context.m_wait_for_fence(value);
            }};
        }
        {
            PPR_DEFER {
                pass->end();
            };

            const DrawContext scene_context{
                .m_device = context.m_device,
                .m_pass = *pass,
                .m_render_pipeline_key = scene_key,
                .m_viewport = rhi::Viewport::fromSize(static_cast<float>(extent.x), static_cast<float>(extent.y)),
                .m_scissor = rhi::ScissorRect::fromSize(safe_narrowing(extent.x), safe_narrowing(extent.y)),
                .m_target_extent = extent,
                .m_completed_fence_value = effective_completed,
                .m_wait_for_fence = std::move(scene_wait),
            };
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, encode_scene(scene_context));
        }

        context.m_encoder.setTextureState(slot.m_color, target_range, rhi::ResourceState::ShaderResource);

        m_scene_ready = true;
        m_scene_slot = selected;
        m_scene_recorded = true;
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // surface present
    // ------------------------------------------------------------------

    std::error_code RetroPass::ensurePipeline_(rhi::IDevice &device, const RenderPipelineSignature &signature) {
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_program != nullptr ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        if (m_pipeline_key and static_cast<const RenderPipelineSignature &>(*m_pipeline_key) != signature) {
            if (m_pipeline) {
                m_deferred_releases.defer(
                    m_last_submitted_value, [held = std::move(m_pipeline)]() mutable { held.setNull(); });
            }
            m_pipeline_key.reset();
        }
        if (not m_pipeline) {
            rhi::ColorTargetDesc target{};
            target.format = signature.colorFormats().front();
            target.enableBlend = false;
            rhi::RenderPipelineDesc desc{};
            desc.program = m_program.get();
            desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            desc.targets = &target;
            desc.targetCount = 1u;
            desc.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);
            desc.depthStencil.format = rhi::Format::Undefined;
            desc.depthStencil.depthTestEnable = false;
            desc.depthStencil.depthWriteEnable = false;
            desc.rasterizer.cullMode = rhi::CullMode::None;
            desc.label = "retro present pipeline";
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createRenderPipeline(desc, m_pipeline.writeRef()));
            m_pipeline_key.emplace(signature);
        }

        return default_value_v;
    }

    std::error_code RetroPass::render(const DrawContext &context) {
        const hal::ProfileScope scope{"App.Pass.Retro.Present"};

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_scene_ready and m_scene_slot < kFrameRingSlots and m_scene_slots[m_scene_slot].m_color_view
                           ? std::error_code{}
                           : std::make_error_code(std::errc::operation_not_permitted));
        std::ignore = m_deferred_releases.reclaim(context.m_completed_fence_value);

        // The present pipeline targets the SURFACE: exactly one color format
        // and no depth-stencil format, because the retro scene already
        // resolved depth into its offscreen target.
        const RenderPipelineSignature &surface_signature = context.m_render_pipeline_key;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            surface_signature.colorFormats().size() == 1u and not surface_signature.m_depth_stencil_format.has_value()
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, ensurePipeline_(context.m_device, surface_signature));

        const SceneTarget &slot = m_scene_slots[m_scene_slot];
        RetroParameters parameters{};
        for (u32 index = 0u; index < kPaletteSize; ++index) {
            parameters.m_palette[index][0] = m_palette[index].x;
            parameters.m_palette[index][1] = m_palette[index].y;
            parameters.m_palette[index][2] = m_palette[index].z;
            parameters.m_palette[index][3] = m_palette[index].w;
        }
        parameters.m_source_extent[0] = static_cast<float>(slot.m_extent.x);
        parameters.m_source_extent[1] = static_cast<float>(slot.m_extent.y);
        parameters.m_dither_strength = kDitherStrength;
        parameters.m_reserved = 0u;

        rhi::IShaderObject *const root = m_scene_roots[m_scene_slot].get();
        rhi::ShaderCursor cursor{root};
        const rhi::ShaderCursor retro_field = cursor["g_retro"];
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, retro_field.isValid() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        rhi::ShaderCursor retro_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, retro_field.getDereferenced(retro_cursor));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, retro_cursor.setData(&parameters, sizeof(parameters)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_scene"].setBinding(rhi::Binding(slot.m_color_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_sampler"].setDescriptorHandle(m_sampler_handle));

        context.m_pass.setRenderState({
            .viewports = {context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {context.m_scissor},
            .scissorRectCount = 1u,
            .vertexBuffers = {},
            .indexBuffer = {},
        });
        context.m_pass.bindPipeline(m_pipeline, root);
        context.m_pass.draw({.vertexCount = 3u});

        // The source target is consumed by this single draw; a later frame
        // prepares it again before it is sampled.
        m_scene_ready = false;
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // frame settlement
    // ------------------------------------------------------------------

    void RetroPass::notifyFrameSubmitted(const FenceCompletionValue submit_value) noexcept {
        m_last_submitted_value = submit_value;
        if (m_scene_recorded) {
            // The frame that rendered into the slot is (or is about to be)
            // submitted under this value: the slot is reusable only once the
            // fence passes it.
            m_scene_slots[m_scene_slot].m_last_use = submit_value;
            m_scene_recorded = false;
        }
    }
}
