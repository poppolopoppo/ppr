module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;
import :renderer.grid_pass;
import engine.core;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    // ------------------------------------------------------------------
    // fluid programs and scalar compute protocol
    // ------------------------------------------------------------------

    namespace {
        constexpr u32 kFluidMaterial = 0u;
        constexpr u32 kFluidDepth = 1u;
        constexpr u32 kFluidRaw = 2u;
        constexpr u32 kFluidFiltered = 3u;

        struct FluidFilterFrame {
            u32 m_width{}, m_height{};
            u32 m_reserved[2]{};
        };

        static_assert(sizeof(FluidFilterFrame) == 16u and alignof(FluidFilterFrame) == 4u);
        static_assert(PPR_OFFSETOF(FluidFilterFrame, m_height) == 4u);

        rhi::BufferRange fluidBufferRange(rhi::IBuffer &buffer) noexcept {
            return {.offset = 0u, .size = buffer.getDesc().size};
        }
    }

    std::error_code GridPass::createFluidPrograms_(
        IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir) {
        shader::SharedModule splats, filter;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            service.loadModuleFromFile(content_dir / "shaders" / "grid_tiles.slang", "grid_tiles", splats.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, service.loadModuleFromFile(
                          content_dir / "shaders" / "grid_fluid_filter.slang", "grid_fluid_filter", filter.writeRef()));

        auto *const layout = filter->getLayout();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, layout != nullptr ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        auto *const frame = layout->findTypeByName("FluidFilterFrame");
        auto *const frame_layout = frame != nullptr ? layout->getTypeLayout(frame) : nullptr;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, frame_layout != nullptr and
                              frame_layout->getSize() == sizeof(FluidFilterFrame) and
                              frame_layout->getFieldByIndex(1u)->getOffset() == PPR_OFFSETOF(FluidFilterFrame, m_height)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        constexpr std::array<const char *, 4u> vertices{
            "vertexFluidSplat", "vertexFluidSplat", "filterLiquid", "vertexFluidComposite"};
        constexpr std::array<const char *, 4u> fragments{
            "fragmentFluidMaterial", "fragmentFluidThickness", nullptr, "fragmentFluidComposite"};
        for (u32 index = 0u; index < m_fluid_programs.size(); ++index) {
            auto *const module = index == 2u ? filter.get() : splats.get();
            shader::ComPtr<slang::IEntryPoint> vertex, fragment;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(vertices[index], vertex.writeRef()));
            if (fragments[index] != nullptr) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(fragments[index], fragment.writeRef()));
            }

            slang::IComponentType *entries[]{vertex.get(), fragment.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module;
            desc.slangEntryPoints = entries;
            desc.slangEntryPointCount = fragment ? 2u : 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createShaderProgram(desc, m_fluid_programs[index].writeRef(), diagnostics.writeRef()));
        }

        for (u32 index = 0u; index < m_fluid_splat_pipelines.size(); ++index) {
            rhi::ColorTargetDesc color{};
            color.format = index == 0u ? rhi::Format::R32Uint : rhi::Format::R32Float;
            color.enableBlend = index == 1u;
            color.color.srcFactor = rhi::BlendFactor::One;
            color.color.dstFactor = rhi::BlendFactor::One;
            color.color.op = rhi::BlendOp::Add;
            color.alpha = color.color;

            rhi::RenderPipelineDesc desc{};
            desc.program = m_fluid_programs[index];
            desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            desc.targets = &color;
            desc.targetCount = 1u;
            desc.depthStencil.format = index == 0u ? rhi::Format::D32Float : rhi::Format::Undefined;
            desc.depthStencil.depthTestEnable = index == 0u;
            desc.depthStencil.depthWriteEnable = index == 0u;
            desc.depthStencil.depthFunc = rhi::ComparisonFunc::Less;
            desc.rasterizer.cullMode = rhi::CullMode::None;
            desc.label = index == 0u ? "fluid element selection" : "fluid kernel accumulation";
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createRenderPipeline(desc, m_fluid_splat_pipelines[index].writeRef()));
        }

        rhi::ComputePipelineDesc compute{};
        compute.program = m_fluid_programs[2u];
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createComputePipeline(compute, m_fluid_filter_pipeline.writeRef()));
        return {};
    }

    // ------------------------------------------------------------------
    // completion-selected fluid attachments and reconstruction
    // ------------------------------------------------------------------

    bool GridPass::hasLiquidParticles_() const noexcept {
        if (not m_world.m_gpu_particles) {
            return false;
        }

        const auto &materials = m_world.m_gpu_particles->m_view.m_metadata->m_materials;
        return std::ranges::any_of(m_world.m_terrain_visuals, [&](const GridTerrainVisual &visual) {
            return (visual.m_surface_flags & kGridSurfaceParticleLiquid) != 0u and
                   std::ranges::find(materials, visual.m_material_id) != materials.end();
        });
    }

    std::error_code GridPass::prepareFluidSurface_(const PreRenderContext &context) {
        if (m_world.m_diagnostic_mode != EGridDiagnosticMode::textured or not hasLiquidParticles_()) {
            return {};
        }

        const hal::ProfileScope scope{"App.Grid.Liquids.Reconstruct"};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, std::isfinite(m_background_viewport.extentX) and
                              std::isfinite(m_background_viewport.extentY) and
                              m_background_viewport.extentX > 0.0f and
                              m_background_viewport.extentY > 0.0f
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        // Reconstruct at half resolution, bounding the longest field edge while
        // preserving aspect ratio. Native shading and temporal output keep the
        // full scene extent; field-texel footprints drive silhouette AA/normals.
        const float longest_edge = std::max(m_background_viewport.extentX, m_background_viewport.extentY);
        const float field_scale = std::min(0.5f, 1280.0f / longest_edge);
        const u32 width = static_cast<u32>(
            std::clamp(std::ceil(m_background_viewport.extentX * field_scale), 1.0f, 1280.0f));
        const u32 height = static_cast<u32>(
            std::clamp(std::ceil(m_background_viewport.extentY * field_scale), 1.0f, 1280.0f));
        auto &storage = *m_world.m_gpu_particles;
        auto &slot = storage.m_slots[storage.m_selected_slot];
        if (not slot.m_fluid or slot.m_fluid->m_width != width or slot.m_fluid->m_height != height) {
            // selectSlot_ already established completion of this exact slot.
            // Build all replacements before dropping its still-usable cohort.
            auto replacement = std::make_unique<FluidSlot>();
            replacement->m_width = width;
            replacement->m_height = height;
            for (u32 index = 0u; index < replacement->m_textures.size(); ++index) {
                rhi::TextureDesc desc{};
                desc.type = rhi::TextureType::Texture2D;
                desc.size = {.width = width, .height = height, .depth = 1u};
                desc.arrayLength = 1u;
                desc.mipCount = 1u;
                desc.format = index == kFluidMaterial
                                  ? rhi::Format::R32Uint
                                  : (index == kFluidDepth ? rhi::Format::D32Float : rhi::Format::R32Float);
                desc.memoryType = rhi::MemoryType::DeviceLocal;
                desc.usage = index == kFluidDepth ? rhi::TextureUsage::DepthStencil
                                                  : static_cast<rhi::TextureUsage>(
                                                        static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                                                        static_cast<u32>(
                                                            index == kFluidFiltered ? rhi::TextureUsage::UnorderedAccess
                                                                                    : rhi::TextureUsage::RenderTarget));
                desc.defaultState = index == kFluidDepth ? rhi::ResourceState::DepthWrite
                                                         : rhi::ResourceState::ShaderResource;
                desc.label = "completion-protected fluid optical field";
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, context.m_device.createTexture(desc, nullptr, replacement->m_textures[index].writeRef()));
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, replacement->m_textures[index]->getDefaultView(replacement->m_views[index].writeRef()));
            }

            for (u32 program = 0u; program < replacement->m_roots.size(); ++program) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, context.m_device.createRootShaderObject(
                                  m_fluid_programs[program], replacement->m_roots[program].writeRef()));
            }

            slot.m_fluid = std::move(replacement);
        }

        auto &fluid = *slot.m_fluid;
        GridFrame frame{};
        frame.m_view_projection = m_camera_view.m_view_projection;
        frame.m_inverse_view_projection = m_camera_view.m_invert_view_projection;
        frame.m_cell_size_m = m_world.m_cell_size_m;
        frame.m_viewport[2] = static_cast<float>(width);
        frame.m_viewport[3] = static_cast<float>(height);
        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        const auto bind_buffer = [&](rhi::ShaderCursor &cursor, const char *const name,
                                     rhi::IBuffer &buffer) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor[name].setBinding(rhi::Binding(&buffer, fluidBufferRange(buffer))));
            return {};
        };

        context.m_encoder.setTextureState(fluid.m_textures[kFluidDepth], range, rhi::ResourceState::DepthWrite);
        for (u32 phase = 0u; phase < 2u; ++phase) {
            context.m_encoder.pushDebugGroup(phase == 0u ? "Liquids.Elements" : "Liquids.Thickness", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };

            rhi::ShaderCursor cursor{fluid.m_roots[phase]};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_grid_frame"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&frame, sizeof(frame)));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer(cursor, "g_particles", *storage.m_packed));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer(cursor, "g_particle_ids", *slot.m_ids));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer(cursor, "g_terrain_visuals", *m_world.m_terrain_palette));
            if (phase == 1u) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor["g_fluid_material"].setBinding(rhi::Binding(fluid.m_views[kFluidMaterial])));
            }

            const u32 target = phase == 0u ? kFluidMaterial : kFluidRaw;
            context.m_encoder.setTextureState(fluid.m_textures[target], range, rhi::ResourceState::RenderTarget);
            rhi::RenderPassColorAttachment color{};
            color.view = fluid.m_views[target];
            rhi::RenderPassDepthStencilAttachment depth{};
            depth.view = fluid.m_views[kFluidDepth];
            depth.depthClearValue = 1.0f;
            rhi::RenderPassDesc desc{};
            desc.colorAttachments = &color;
            desc.colorAttachmentCount = 1u;
            desc.depthStencilAttachment = phase == 0u ? &depth : nullptr;
            auto *const pass = context.m_encoder.beginRenderPass(desc);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };

                pass->setRenderState(
                    {.viewports = {rhi::Viewport::fromSize(static_cast<float>(width), static_cast<float>(height))},
                        .viewportCount = 1u,
                        .scissorRects = {rhi::ScissorRect::fromSize(width, height)},
                        .scissorRectCount = 1u,
                        .vertexBuffers = {},
                        .indexBuffer = {}});
                if (m_particle_timing_queries) {
                    pass->writeTimestamp(m_particle_timing_queries, 2u + phase * 2u);
                }

                pass->bindPipeline(m_fluid_splat_pipelines[phase], fluid.m_roots[phase]);
                pass->drawIndirect(1u, rhi::BufferOffsetPair{slot.m_args});
                if (m_particle_timing_queries) {
                    pass->writeTimestamp(m_particle_timing_queries, 3u + phase * 2u);
                }
                ++m_draw_commands;
            }

            context.m_encoder.setTextureState(fluid.m_textures[target], range, rhi::ResourceState::ShaderResource);
        }

        context.m_encoder.pushDebugGroup("Liquids.Filter", {});
        PPR_DEFER {
            context.m_encoder.popDebugGroup();
        };

        const FluidFilterFrame filter{width, height, {}};
        rhi::ShaderCursor cursor{fluid.m_roots[2u]};
        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_fluid_filter"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&filter, sizeof(filter)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_fluid_material"].setBinding(rhi::Binding(fluid.m_views[kFluidMaterial])));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_fluid_raw"].setBinding(rhi::Binding(fluid.m_views[kFluidRaw])));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_fluid_filtered"].setBinding(rhi::Binding(fluid.m_views[kFluidFiltered])));
        context.m_encoder.setTextureState(fluid.m_textures[kFluidFiltered], range, rhi::ResourceState::UnorderedAccess);
        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        {
            PPR_DEFER {
                pass->end();
            };

            if (m_particle_timing_queries) {
                pass->writeTimestamp(m_particle_timing_queries, 6u);
            }

            pass->bindPipeline(m_fluid_filter_pipeline, fluid.m_roots[2u]);
            pass->dispatchCompute((width + 7u) / 8u, (height + 7u) / 8u, 1u);
            if (m_particle_timing_queries) {
                pass->writeTimestamp(m_particle_timing_queries, 7u);
            }
            ++m_dispatch_commands;
        }

        context.m_encoder.setTextureState(fluid.m_textures[kFluidFiltered], range, rhi::ResourceState::ShaderResource);
        return {};
    }

    // ------------------------------------------------------------------
    // compositing with the scene's terrain and optical material contract
    // ------------------------------------------------------------------

    std::error_code GridPass::renderFluidSurface_(const DrawContext &context, const u32 slot_index) {
        if (not hasLiquidParticles_()) {
            return {};
        }

        const hal::ProfileScope scope{"App.Grid.Liquids.Composite"};
        context.m_pass.pushDebugGroup("Liquids.Composite", {});
        PPR_DEFER {
            context.m_pass.popDebugGroup();
        };

        auto &fluid_ptr = m_world.m_gpu_particles->m_slots[slot_index].m_fluid;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, fluid_ptr ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));
        auto &fluid = *fluid_ptr;
        const auto &signature = static_cast<const RenderPipelineSignature &>(context.m_render_pipeline_key);
        if (m_spatial_msaa) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureSpatialPrograms_(context.m_device));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, signature.m_sample_count == SampleCount::x4
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
        }

        // This completed particle slot owns the final root family and its
        // field/backdrop leases. Geometry/reconstruction roots remain separate.
        const auto pipeline = opticalPipeline_(
            context.m_device, signature, m_spatial_msaa ? m_fluid_spatial_program.get() : m_fluid_programs[3u].get(),
            fluid.m_composite_key, fluid.m_composite_pipeline, true, m_spatial_msaa);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, pipeline);

        auto *const root = (*pipeline)->m_slot_roots[slot_index].get();
        rhi::ShaderCursor cursor{root};
        rhi::ShaderCursor frame;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_grid_frame"].getDereferenced(frame));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_(frame));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, frame["m_inverse_view_projection"].setData(
                          &m_camera_view.m_invert_view_projection, sizeof(m_camera_view.m_invert_view_projection)));
        const float viewport[]{
            context.m_viewport.originX, context.m_viewport.originY, context.m_viewport.extentX,
            context.m_viewport.extentY};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, frame["m_viewport"].setData(viewport, sizeof(viewport)));

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, bindSurfaceBuffers_(context.m_device, cursor, m_surface_retained_table, *pipeline, slot_index));
        const bool has_cells = m_surface_retained_table ? m_world.m_visibility_has_cells
                                                        : m_world.m_cell_atlas != nullptr;
        auto *const cells = has_cells ? m_world.m_cell_atlas.get() : m_world.m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_cell_materials"].setBinding(rhi::Binding(cells, fluidBufferRange(*cells))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_fluid_material"].setBinding(rhi::Binding(fluid.m_views[kFluidMaterial])));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_fluid_surface"].setBinding(rhi::Binding(fluid.m_views[kFluidFiltered])));

        context.m_pass.setRenderState(
            {.viewports = {context.m_viewport},
                .viewportCount = 1u,
                .scissorRects = {context.m_scissor},
                .scissorRectCount = 1u,
                .vertexBuffers = {},
                .indexBuffer = {}});
        if (m_particle_timing_queries) {
            context.m_pass.writeTimestamp(m_particle_timing_queries, 8u);
        }

        context.m_pass.bindPipeline((*pipeline)->m_pipeline, root);
        context.m_pass.draw({.vertexCount = 3u});
        if (m_particle_timing_queries) {
            context.m_pass.writeTimestamp(m_particle_timing_queries, 9u);
        }
        ++m_draw_commands;
        return {};
    }
}
