module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;
import :renderer.grid_pass;
import :renderer.world_lighting;
import :renderer.world_lighting_pass;
import std;
import engine.core;
import engine.rhi;
import engine.shader;

namespace pP {
    using pP::make_error_code;

    // ------------------------------------------------------------------
    // screen-space particle surface: GridPass-owned resources and passes
    // ------------------------------------------------------------------
    // One reconstruction method over every admitted phase, over the same prepared
    // rows/indirect arguments the direct classes use, at full raster resolution:
    //
    //   label selection  L = R32Uint winning material, Z = D32Float minimum
    //                    eligible rho^2. No blend, strict Less, depth writes.
    //   accumulation     Q = (sum w*base.rgb, sum w), G = (sum w*normal.xyz,
    //                    sum w*alpha). One/One/Add, no depth attachment.
    //   composite        one categorical four-tap reconstruction and one deferred
    //                    response evaluation per surviving scene sample, drawn at
    //                    each admitted class's existing depth/blend slot.
    //
    // There is no filter, no density history, no dirty-tile cache, no readback and
    // no temporal smoothing. Every target clears to its exact empty value every
    // rendered frame.

    namespace {
        rhi::BufferRange particleSurfaceRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        // CPU mirror of grid_tiles.slang's ParticleSurfaceFrame. Field order,
        // widths and offsets are asserted where the program is created.
        struct ParticleSurfaceConstants {
            float m_pixel_world{};
            u32 m_phase_mask{};
            u32 m_composite_class{};
            u32 m_pad{};
        };
        static_assert(sizeof(ParticleSurfaceConstants) == 16u and alignof(ParticleSurfaceConstants) == 4u);
        static_assert(PPR_OFFSETOF(ParticleSurfaceConstants, m_phase_mask) == 4u);
        static_assert(PPR_OFFSETOF(ParticleSurfaceConstants, m_composite_class) == 8u);
    }

    // ------------------------------------------------------------------
    // admission and target lifetime
    // ------------------------------------------------------------------

    void GridPass::deriveParticleSurfaceLane_() noexcept {
        // Admission is a phase mask over the publication's material inventory,
        // not a single material id. Every valid liquid material is admitted
        // together, including a material with no visible row: an offscreen
        // competitor still owns texels the moment it becomes visible, and a
        // one-material field would then change representation mid-frame.
        m_world.m_particle_surface = {};
        if (m_world.m_diagnostic_mode != EGridDiagnosticMode::textured or not m_world.m_gpu_particles) {
            return;
        }

        // The lane must describe the rows this frame draws. A current publication
        // is about to be packed, so its incoming metadata is authoritative; a hold
        // draws the retained accepted rows, so only the accepted packed owner is.
        // The incoming view may be a rejected attempt's inventory while the packed
        // rows are still the previous publication's, which is why the owner is
        // committed with the pack instead of inferred from the latest view.
        const auto &retained = m_world.m_gpu_particles->m_packed_metadata_owner;
        const auto &metadata = retainedGpuParticleCurrent_() ? m_world.m_gpu_particles->m_view.m_metadata : retained;
        if (not metadata) {
            return;
        }

        ParticleSurfaceLane lane{};
        for (const u32 material: metadata->m_materials) {
            const auto visual = std::ranges::find(
                m_world.m_terrain_visuals, material, &GridTerrainVisual::m_material_id);
            if (visual == m_world.m_terrain_visuals.end()) {
                continue;
            }

            // The shader's own phase rule: liquid is authored liquid without the
            // solid bit. A material authored as both is solid, and every other
            // material keeps its direct class.
            const bool liquid = (visual->m_surface_flags & kGridSurfaceParticleLiquid) != 0u;
            const bool solid = (visual->m_surface_flags & kGridSurfaceParticleSolid) != 0u;
            if (not liquid or solid) {
                continue;
            }

            lane.m_phase_mask |= kParticleSurfacePhaseLiquid;
            const auto draw_class = (visual->m_surface_flags & kGridSurfaceOpticalBlend) != 0u
                                        ? EGridParticleDrawClass::optical
                                        : EGridParticleDrawClass::opaque;
            lane.m_class_mask |= 1u << static_cast<u32>(draw_class);
            ++lane.m_material_count;
        }

        if (not lane.admitted()) {
            return;
        }

        m_world.m_particle_surface = lane;
        PPR_LOG(
            GridPass, debug, "particle surface lane admitted",
            {
                {"liquid_materials", lane.m_material_count},
                {"publication_materials", metadata->m_materials.size()},
                {"class_mask", lane.m_class_mask},
            });
    }

    std::error_code GridPass::ensureParticleSurfaceTarget_(
        rhi::IDevice &device, ParticleSurfaceSlot &slot, const u32 width, const u32 height) {
        if (slot.m_label and slot.m_width == width and slot.m_height == height) {
            return {};
        }

        // The caller proved this slot's last reader completed before selecting
        // it, so replacing the bundle cannot race an in-flight composite or
        // accumulation.
        slot.m_label.setNull();
        slot.m_label_view.setNull();
        slot.m_depth.setNull();
        slot.m_depth_view.setNull();
        slot.m_base.setNull();
        slot.m_base_view.setNull();
        slot.m_normal.setNull();
        slot.m_normal_view.setNull();

        rhi::TextureDesc desc{};
        desc.type = rhi::TextureType::Texture2D;
        desc.size = {.width = width, .height = height, .depth = 1u};
        desc.arrayLength = 1u;
        desc.mipCount = 1u;
        desc.format = rhi::Format::R32Uint;
        desc.memoryType = rhi::MemoryType::DeviceLocal;
        desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "grid particle surface label";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, slot.m_label.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, slot.m_label->getDefaultView(slot.m_label_view.writeRef()));

        // Selection depth: the synthetic rho^2 distance, never scene depth and
        // never sampled at composite.
        desc.format = rhi::Format::D32Float;
        desc.usage = rhi::TextureUsage::DepthStencil;
        desc.defaultState = rhi::ResourceState::DepthWrite;
        desc.label = "grid particle surface selection depth";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, slot.m_depth.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, slot.m_depth->getDefaultView(slot.m_depth_view.writeRef()));

        // Q and G: the two attribute sums. RGBA16F carries signed normal
        // components and the existing additive blend contract.
        desc.format = rhi::Format::RGBA16Float;
        desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "grid particle surface base+density (Q)";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, slot.m_base.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, slot.m_base->getDefaultView(slot.m_base_view.writeRef()));

        desc.label = "grid particle surface normal+alpha (G)";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, slot.m_normal.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, slot.m_normal->getDefaultView(slot.m_normal_view.writeRef()));

        slot.m_width = width;
        slot.m_height = height;
        return {};
    }

    float GridPass::particleSurfacePixelWorld_(const CameraSnapshot &camera) const noexcept {
        // Orthographic admission: the camera's own scale is already world metres
        // per raster pixel, so the quad padding and the cull-plane guard are the
        // same number with no projection maths.
        if (camera.m_camera_mode == ECameraProjection::orthographic and
            std::isfinite(camera.m_ortho_scale) and
            camera.m_ortho_scale > 0.0f) {
            return camera.m_ortho_scale;
        }

        const float2 extent = camera.m_viewport_size;
        if (not (extent.x > 0.0f) or not (extent.y > 0.0f)) {
            return 0.0f;
        }

        // Perspective: the world size of a pixel at the z = 0 plane varies across
        // the raster, so take the largest of the centre and the four edge
        // midpoints. Conservatism here only widens admission; the kernel weight
        // still vanishes outside the analytic support.
        const auto plane_point = [&camera](const float2 ndc) noexcept {
            const float4 near_h = float4{ndc.x, ndc.y, 0.0f, 1.0f} * camera.m_invert_view_projection;
            const float4 far_h = float4{ndc.x, ndc.y, 1.0f, 1.0f} * camera.m_invert_view_projection;
            if (near_h.w == 0.0f or far_h.w == 0.0f) {
                return float2{0.0f, 0.0f};
            }

            const float3 near_point = near_h.xyz / near_h.w;
            const float3 direction = far_h.xyz / far_h.w - near_point;
            const float t = std::abs(direction.z) > 1.0e-8f ? -near_point.z / direction.z : 0.0f;
            return float2{near_point.x + direction.x * t, near_point.y + direction.y * t};
        };

        const float2 pixel_ndc{2.0f / extent.x, 2.0f / extent.y};
        const std::array<float2, 5u> samples{
            float2{0.0f, 0.0f}, float2{-1.0f, 0.0f}, float2{1.0f, 0.0f}, float2{0.0f, -1.0f}, float2{0.0f, 1.0f}};
        float guard = 0.0f;
        for (const float2 &ndc: samples) {
            const float2 base = plane_point(ndc);
            const float2 along_x = plane_point(ndc + float2{pixel_ndc.x, 0.0f}) - base;
            const float2 along_y = plane_point(ndc + float2{0.0f, pixel_ndc.y}) - base;
            guard = std::max({guard, length(along_x), length(along_y)});
        }

        return std::isfinite(guard) ? guard : 0.0f;
    }

    // ------------------------------------------------------------------
    // programs and signature-keyed pipelines
    // ------------------------------------------------------------------

    std::error_code GridPass::createParticleSurfacePrograms_(
        IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir) {
        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, service.loadModuleFromFile(
                          content_dir / TEXT("shaders") / TEXT("grid_tiles.slang"), "grid_tiles", module.writeRef()));

        // Reflect the lane constant buffer the three passes share. A target that
        // widens a field or reorders the word fails program creation here instead
        // of misreading the admitted phase mask or the composite class.
        auto *const layout = module->getLayout();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, layout != nullptr ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        slang::TypeLayoutReflection *lane_frame{};
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_particle_surface") {
                lane_frame = parameter->getTypeLayout()->getElementTypeLayout();
                break;
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, lane_frame != nullptr and
                              lane_frame->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ParticleSurfaceConstants)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 3u>{
                 std::pair{"m_pixel_world", 0u}, {"m_phase_mask", 4u}, {"m_composite_class", 8u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < lane_frame->getFieldCount(); ++index) {
                auto *const field = lane_frame->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    matched = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, matched ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        const auto create = [&](const char *const vertex, const char *const fragment,
                                rhi::ComPtr<rhi::IShaderProgram> &program) -> std::error_code {
            shader::ComPtr<slang::IEntryPoint> vertex_ep, fragment_ep;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(vertex, vertex_ep.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(fragment, fragment_ep.writeRef()));
            slang::IComponentType *entries[] = {vertex_ep.get(), fragment_ep.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module.get();
            desc.slangEntryPoints = entries;
            desc.slangEntryPointCount = 2u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createShaderProgram(desc, program.writeRef(), diagnostics.writeRef()));
            return {};
        };

        // Selection is a separate program: it writes the integer label plus
        // SV_Depth and must not blend. Accumulation and the two composites are
        // separate programs so each entry declares only the inputs it reads.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            create("vertexParticleSurfaceSplat", "fragmentParticleSurfaceSelect", m_particle_surface_select_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceSplat", "fragmentParticleSurfaceAccumulate",
                          m_particle_surface_accumulate_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceComposite", "fragmentParticleSurfaceComposite",
                          m_particle_surface_composite_program));
        return {};
    }

    Expected<GridPass::GridCachedPipeline *> GridPass::particleSurfacePipeline_(
        rhi::IDevice &device, const RenderPipelineSignature &signature, rhi::IShaderProgram *const program,
        std::optional<RenderPipelineKey> &key, std::optional<GridCachedPipeline> &cache,
        const EParticleSurfacePipelineState state, const bool depth_write, const char *const label) {
        const auto formats = signature.colorFormats();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, program != nullptr and not formats.empty() and formats.size() <= 3u
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        if (cache and ((key and static_cast<const RenderPipelineSignature &>(*key) != signature) or
                          cache->m_pipeline->getProgram() != program)) {
            if (cache) {
                m_deferred_releases.defer(m_last_submitted_value, [held = std::move(*cache)]() mutable { held = {}; });
                cache.reset();
            }
            key.reset();
        }

        if (not cache) {
            // Two attribute targets today; the third entry is the solid phase's
            // thermal attachment when it migrates, so the local capacity is three
            // and the generic renderer's four-colour limit is not touched.
            std::array<rhi::ColorTargetDesc, 3u> targets{};
            for (u32 index = 0u; index < formats.size(); ++index) {
                auto &target = targets[index];
                target.format = formats[index];
                switch (state) {
                    case EParticleSurfacePipelineState::selection:
                        // Integer label: no blend, full write mask. Depth is the
                        // synthetic rho^2 selection distance.
                        target.enableBlend = false;
                        break;
                    case EParticleSurfacePipelineState::attributes:
                        // One/One/Add on every component: the accumulation adds
                        // this fragment's (w*base, w[, w*normal, w*alpha]) and
                        // never reads the target.
                        target.enableBlend = true;
                        target.color = {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOp::Add};
                        target.alpha = target.color;
                        break;
                    case EParticleSurfacePipelineState::composite:
                        // The composite blends premultiplied once: C*P over the scene.
                        target.enableBlend = true;
                        target.color = {rhi::BlendFactor::One, rhi::BlendFactor::InvSrcAlpha, rhi::BlendOp::Add};
                        target.alpha = target.color;
                        break;
                }
            }

            const bool selection = state == EParticleSurfacePipelineState::selection;
            const bool depth_test = selection or state == EParticleSurfacePipelineState::composite;
            rhi::RenderPipelineDesc desc{};
            desc.program = program;
            desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            desc.targets = targets.data();
            desc.targetCount = static_cast<u32>(formats.size());
            desc.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);
            desc.depthStencil.format = signature.m_depth_stencil_format.value_or(rhi::Format::Undefined);
            desc.depthStencil.depthTestEnable = depth_test and signature.m_depth_stencil_format.has_value();
            desc.depthStencil.depthWriteEnable = desc.depthStencil.depthTestEnable and (selection or depth_write);
            // Selection is the nearest-footprint test (strict Less, ties keep the
            // first stable instance); the composite shares the scene plane depth
            // with the coplanar terrain and particles (LessEqual).
            desc.depthStencil.depthFunc = selection ? rhi::ComparisonFunc::Less : rhi::ComparisonFunc::LessEqual;
            desc.rasterizer.cullMode = rhi::CullMode::None;
            desc.label = label;

            GridCachedPipeline value;
            const auto result = device.createRenderPipeline(desc, value.m_pipeline.writeRef());
            const auto pipeline_error = rhi::make_error_code(result);
            if (pipeline_error) {
                const auto message = std::format(
                    "Grid particle-surface pipeline creation failed result={} category={} cause={} label={} "
                    "entries={} samples={} targets={} depth_format={} state={} depth_test={} depth_write={}",
                    static_cast<int>(result), pipeline_error.category().name(), pipeline_error.message(), label,
                    program->getDesc().slangEntryPointCount, desc.multisample.sampleCount, desc.targetCount,
                    static_cast<int>(desc.depthStencil.format), static_cast<int>(state),
                    desc.depthStencil.depthTestEnable, desc.depthStencil.depthWriteEnable);
                PPR_LOG_RAW(GridPass, error, message);
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, pipeline_error);
            cache.emplace(std::move(value));
            key.emplace(signature);
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, ensurePipelineRoots_(device, *cache));
        return &*cache;
    }

    // ------------------------------------------------------------------
    // pre-render selection and accumulation
    // ------------------------------------------------------------------

    std::error_code GridPass::encodeParticleSurfacePasses_(
        const PreRenderContext &context, ParticleSurfaceSlot &slot, const u32 slot_index, const rhi::Viewport &viewport,
        const rhi::ScissorRect &scissor) {
        // The prepared draw source is required: a hold presents its retained rows,
        // and an admitted field without them is a caller error rather than an empty
        // draw. An unprepared publication (no rows ever packed) legitimately draws
        // nothing and both passes still clear.
        const bool draw = m_world.m_gpu_particles and m_world.m_gpu_particles->m_prepared;
        rhi::IBuffer *packed{};
        rhi::IBuffer *orientation{};
        rhi::IBuffer *conic{};
        rhi::IBuffer *ids{};
        rhi::IBuffer *args{};
        if (draw) {
            auto &storage = *m_world.m_gpu_particles;
            auto &particle_slot = storage.m_slots[storage.m_selected_slot];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, storage.m_packed and particle_slot.m_ids and particle_slot.m_args
                              ? std::error_code{}
                              : std::make_error_code(std::errc::state_not_recoverable));
            packed = storage.m_packed;
            orientation = storage.m_orientation_draw.get();
            conic = particle_slot.m_conic.get();
            ids = particle_slot.m_ids;
            args = particle_slot.m_args;
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, orientation != nullptr and conic != nullptr
                              ? std::error_code{}
                              : std::make_error_code(std::errc::state_not_recoverable));
        }

        // Every root that this frame's two passes use is fully rebound: roots are
        // per pipeline AND per slot, so a slot a given pass has never visited
        // inherits nothing.
        const auto bind_common = [&](GridCachedPipeline &pipeline, rhi::ShaderCursor &cursor) -> std::error_code {
            u32 availability{};
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bindParticleSurfaceSplat_(context.m_device, cursor, availability));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, uploadFrameConstants_(pipeline.m_slot_frame_cursors[slot_index], availability));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bindGridPass_(cursor, 0u, EGridParticleDrawClass::opaque));

            ParticleSurfaceConstants constants{};
            constants.m_pixel_world = m_particle_surface_pixel_world;
            constants.m_phase_mask = m_world.m_particle_surface.m_phase_mask;
            constants.m_composite_class = 0u;
            const rhi::ShaderCursor lane_field = cursor["g_particle_surface"];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                lane_field.isValid() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            rhi::ShaderCursor lane_cursor{};
            PPR_RETURN_ERROR_ON_FAIL(GridPass, lane_field.getDereferenced(lane_cursor));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, lane_cursor.setData(&constants, sizeof(constants)));
            return {};
        };

        const auto bind_draw_source = [&](rhi::ShaderCursor &cursor) -> std::error_code {
            if (not draw) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particles"].setBinding(rhi::Binding(packed, particleSurfaceRange(packed))));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_orientation"].setBinding(
                              rhi::Binding(orientation, particleSurfaceRange(orientation))));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_conic"].setBinding(rhi::Binding(conic, particleSurfaceRange(conic))));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_ids"].setBinding(rhi::Binding(ids, particleSurfaceRange(ids))));
            return {};
        };

        constexpr rhi::SubresourceRange whole{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        const auto state = [&](rhi::ITexture *const texture, const rhi::ResourceState resource_state) {
            if (texture) {
                context.m_encoder.setTextureState(texture, whole, resource_state);
            }
        };

        // --- selection -------------------------------------------------
        RenderPipelineSignature selection_signature{};
        selection_signature.m_color_formats[0] = rhi::Format::R32Uint;
        selection_signature.m_color_format_count = 1u;
        selection_signature.m_depth_stencil_format = rhi::Format::D32Float;
        selection_signature.m_sample_count = SampleCount::x1;
        const auto selection = particleSurfacePipeline_(
            context.m_device, selection_signature, m_particle_surface_select_program.get(),
            m_particle_surface_select_key, m_particle_surface_select_pipeline, EParticleSurfacePipelineState::selection,
            true, "grid particle surface selection pipeline");
        PPR_RETURN_ERROR_ON_FAIL(GridPass, selection);

        state(slot.m_label.get(), rhi::ResourceState::RenderTarget);
        state(slot.m_depth.get(), rhi::ResourceState::DepthWrite);

        rhi::RenderPassColorAttachment label_attachment{};
        label_attachment.view = slot.m_label_view;
        label_attachment.loadOp = rhi::LoadOp::Clear;
        label_attachment.storeOp = rhi::StoreOp::Store;
        // Empty texel: the default zero clear means no material owns it and no
        // composite reconstructs there.
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = slot.m_depth_view;
        depth_attachment.depthLoadOp = rhi::LoadOp::Clear;
        depth_attachment.depthStoreOp = rhi::StoreOp::Store;
        depth_attachment.depthClearValue = 1.0f;

        {
            rhi::RenderPassDesc pass_desc{};
            pass_desc.colorAttachments = &label_attachment;
            pass_desc.colorAttachmentCount = 1u;
            pass_desc.depthStencilAttachment = &depth_attachment;
            auto *const pass = context.m_encoder.beginRenderPass(pass_desc);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->setRenderState(
                {.viewports = {viewport},
                    .viewportCount = 1u,
                    .scissorRects = {scissor},
                    .scissorRectCount = 1u,
                    .vertexBuffers = {},
                    .indexBuffer = {}});

            rhi::ShaderCursor cursor{(*selection)->m_slot_roots[slot_index].get()};
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_common(**selection, cursor));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_draw_source(cursor));
            pass->bindPipeline((*selection)->m_pipeline, (*selection)->m_slot_roots[slot_index].get());
            markPipelineUse_(**selection, slot_index);
            if (draw) {
                pass->drawIndirect(1u, rhi::BufferOffsetPair{args});
                ++m_draw_commands;
            }

            pass->end();
        }
        if (draw and m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 3u);
        }

        // The completed label is read by every accumulation fragment.
        state(slot.m_label.get(), rhi::ResourceState::ShaderResource);

        // --- accumulation ----------------------------------------------
        RenderPipelineSignature attribute_signature{};
        attribute_signature.m_color_formats[0] = rhi::Format::RGBA16Float;
        attribute_signature.m_color_formats[1] = rhi::Format::RGBA16Float;
        attribute_signature.m_color_format_count = 2u;
        attribute_signature.m_sample_count = SampleCount::x1;
        const auto accumulation = particleSurfacePipeline_(
            context.m_device, attribute_signature, m_particle_surface_accumulate_program.get(),
            m_particle_surface_accumulate_key, m_particle_surface_accumulate_pipeline,
            EParticleSurfacePipelineState::attributes, false, "grid particle surface attribute pipeline");
        PPR_RETURN_ERROR_ON_FAIL(GridPass, accumulation);

        state(slot.m_base.get(), rhi::ResourceState::RenderTarget);
        state(slot.m_normal.get(), rhi::ResourceState::RenderTarget);

        // Both attribute targets clear to exact zero every rendered frame; a
        // cleared target is the empty field, never last frame's sums.
        std::array<rhi::RenderPassColorAttachment, 2u> attributes{};
        attributes[0].loadOp = rhi::LoadOp::Clear;
        attributes[0].storeOp = rhi::StoreOp::Store;
        attributes[0].view = slot.m_base_view;
        attributes[1].loadOp = rhi::LoadOp::Clear;
        attributes[1].storeOp = rhi::StoreOp::Store;
        attributes[1].view = slot.m_normal_view;

        {
            rhi::RenderPassDesc pass_desc{};
            pass_desc.colorAttachments = attributes.data();
            pass_desc.colorAttachmentCount = static_cast<u32>(attributes.size());
            auto *const pass = context.m_encoder.beginRenderPass(pass_desc);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->setRenderState(
                {.viewports = {viewport},
                    .viewportCount = 1u,
                    .scissorRects = {scissor},
                    .scissorRectCount = 1u,
                    .vertexBuffers = {},
                    .indexBuffer = {}});

            auto *const root = (*accumulation)->m_slot_roots[slot_index].get();
            rhi::ShaderCursor cursor{root};
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_common(**accumulation, cursor));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_draw_source(cursor));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                slot.m_label_view ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_label"].setBinding(rhi::Binding(slot.m_label_view)));
            pass->bindPipeline((*accumulation)->m_pipeline, root);
            markPipelineUse_(**accumulation, slot_index);
            if (draw) {
                pass->drawIndirect(1u, rhi::BufferOffsetPair{args});
                ++m_draw_commands;
            }

            pass->end();
        }
        if (draw and m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 2u);
        }

        // Q/G are read by both composites; the label stays readable.
        state(slot.m_base.get(), rhi::ResourceState::ShaderResource);
        state(slot.m_normal.get(), rhi::ResourceState::ShaderResource);
        return {};
    }

    std::error_code GridPass::prepareParticleSurface(
        const PreRenderContext &context, const CameraSnapshot &camera, const rhi::Viewport &viewport,
        const rhi::ScissorRect &scissor) {
        // Composite admission is per frame: a field that was not encoded in this
        // submission is never read, and no previous frame's bundle can be shown.
        m_particle_surface_ready = false;
        m_particle_surface_encoded = false;
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }

        const ParticleSurfaceLane lane = m_world.m_particle_surface;
        if (not lane.admitted()) {
            return {};
        }

        // Ensure the shared surface owners before the passes open: a retryable
        // terrain-visual upload must not leave a render pass open, and the passes
        // must never depend on the scene pass having created an owner.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleSurfaceOwners_(context.m_device));

        // Full-raster targets, not the window or the final presentation size.
        const u32 width = scissor.maxX;
        const u32 height = scissor.maxY;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            width != 0u and height != 0u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        // Completion-fenced slot: never write a bundle a submitted frame may
        // still read. The wait fallback only fires past the credit bound.
        std::array<FenceCompletionValue, kTileRingSize> values{};
        for (u32 index = 0u; index < kTileRingSize; ++index) {
            values[index] = m_particle_surface_slots[index].m_last_use;
        }

        const u32 selected = selectRingSlot(context.m_completed_fence_value, values, m_particle_surface_slot);
        if (not slotFenceComplete(context.m_completed_fence_value, values[selected])) [[unlikely]] {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, context.m_wait_for_fence(values[selected]));
        }

        m_particle_surface_slot = selected;
        auto &slot = m_particle_surface_slots[selected];
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleSurfaceTarget_(context.m_device, slot, width, height));

        m_particle_surface_pixel_world = particleSurfacePixelWorld_(camera);

        // Labelled selection, then same-label attributes: both clear their exact
        // empty value every rendered frame, so an empty lane contributes nothing
        // instead of last frame's field.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, encodeParticleSurfacePasses_(context, slot, selected, viewport, scissor));

        m_particle_surface_encoded = true;
        m_particle_surface_ready = true;
        return {};
    }

    // ------------------------------------------------------------------
    // per-scene-sample deferred composite
    // ------------------------------------------------------------------

    std::error_code GridPass::renderParticleSurface(
        const DrawContext &context, const EGridParticleDrawClass draw_class) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particleSurfaceClassAdmitted(draw_class)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::operation_not_permitted));
        if (m_spatial_msaa) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSpatialContent());
        }

        const bool optical = draw_class == EGridParticleDrawClass::optical;
        // Composite state per class: both test LessEqual against the scene plane;
        // only the opaque class writes depth, and only where the coverage floor
        // let the fragment survive. The program is shared; the class is state.
        const auto pipeline = particleSurfacePipeline_(
            context.m_device, context.m_render_pipeline_key, m_particle_surface_composite_program.get(),
            optical ? m_particle_surface_composite_optical_key : m_particle_surface_composite_opaque_key,
            optical ? m_particle_surface_composite_optical_pipeline : m_particle_surface_composite_opaque_pipeline,
            EParticleSurfacePipelineState::composite, not optical,
            optical ? "grid particle surface optical composite pipeline" : "grid particle surface composite pipeline");
        PPR_RETURN_ERROR_ON_FAIL(GridPass, pipeline);

        auto &composite = **pipeline;
        auto *const root = composite.m_slot_roots[m_particle_surface_slot].get();
        rhi::ShaderCursor cursor{root};

        // Shading lives here, so this root carries the deferred response's own
        // inputs and retains the backdrop it samples until the submission
        // completes; the accumulation root has neither.
        auto &owners = composite.m_slot_backdrop_owners[m_particle_surface_slot];
        const auto backdrop_view = m_background_ready ? m_background_slots[m_background_cursor].m_view
                                                      : m_terrain_fallback_view;
        if (std::ranges::none_of(owners, [&](const auto &owner) { return owner.get() == backdrop_view.get(); })) {
            owners.push_back(backdrop_view);
        }

        u32 availability{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindParticleSurfaceInputs_(context.m_device, cursor, availability));
        owners.clear();
        owners.push_back(backdrop_view);
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, uploadFrameConstants_(composite.m_slot_frame_cursors[m_particle_surface_slot], availability));

        ParticleSurfaceConstants constants{};
        constants.m_pixel_world = m_particle_surface_pixel_world;
        constants.m_phase_mask = m_world.m_particle_surface.m_phase_mask;
        constants.m_composite_class = static_cast<u32>(draw_class);
        rhi::ShaderCursor lane_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_surface"].getDereferenced(lane_cursor));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, lane_cursor.setData(&constants, sizeof(constants)));

        const auto &slot = m_particle_surface_slots[m_particle_surface_slot];
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, slot.m_label_view and slot.m_base_view and slot.m_normal_view
                          ? std::error_code{}
                          : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_label"].setBinding(rhi::Binding(slot.m_label_view)));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_base"].setBinding(rhi::Binding(slot.m_base_view)));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_normal"].setBinding(rhi::Binding(slot.m_normal_view)));

        context.m_pass.setRenderState(
            {.viewports = {context.m_viewport},
                .viewportCount = 1u,
                .scissorRects = {context.m_scissor},
                .scissorRectCount = 1u,
                .vertexBuffers = {},
                .indexBuffer = {}});
        context.m_pass.bindPipeline(composite.m_pipeline, root);
        markPipelineUse_(composite, m_particle_surface_slot);
        // Full-raster triangle covering the scene plane; the per-sample fragment
        // reconstructs, thresholds and shades once per surviving sample.
        context.m_pass.draw({.vertexCount = 3u});
        ++m_draw_commands;
        if (m_particle_timing_queries) {
            context.m_pass.writeTimestamp(m_particle_timing_queries, 4u);
        }

        return {};
    }

    std::error_code GridPass::ensureParticleSurfaceOwners_(rhi::IDevice &device) {
        // The same owners `bindMaterialBuffers_` assumes: shared surface buffers,
        // the visible/source palette, the texture inventory with its boundary
        // noise and terrain fallback, and the zero lighting fallback. Every helper
        // is idempotent, so a caller that already prepared the scene pays nothing.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureSurfaceBuffers_(device));
        const auto visuals = prepareTerrainVisuals_(device, 0u);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, visuals);
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, *visuals ? std::error_code{} : std::make_error_code(std::errc::operation_in_progress));
        return ensureLightingFallback_(device);
    }

    std::error_code GridPass::bindParticleSurfaceSplat_(
        rhi::IDevice &device, rhi::ShaderCursor &cursor, u32 &availability) {
        availability = 0u;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleSurfaceOwners_(device));
        // The diagnostic fallback the material set always declares; the surface
        // passes never read it, but a valid descriptor keeps the root complete.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureCellBuffer_(device, false));

        // Every binding is validity-guarded: a resource the linker dropped from
        // this program is skipped instead of dereferencing an invalid cursor. A
        // declared field whose owner was never created is a reported error, never
        // a null binding.
        const auto bind_buffer = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
            const rhi::ShaderCursor field = cursor[name];
            if (not field.isValid()) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                buffer != nullptr ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, field.setBinding(rhi::Binding(buffer, particleSurfaceRange(buffer))));
            return {};
        };
        const auto bind_view = [&](const char *const name, rhi::ITextureView *const view) -> std::error_code {
            const rhi::ShaderCursor field = cursor[name];
            if (not field.isValid()) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, view != nullptr ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, field.setBinding(rhi::Binding(view)));
            return {};
        };
        const auto bind_sampler = [&](const char *const name, rhi::ISampler *const sampler) -> std::error_code {
            const rhi::ShaderCursor field = cursor[name];
            if (not field.isValid()) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                sampler != nullptr ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, field.setBinding(rhi::Binding(sampler)));
            return {};
        };

        // Categorical publication. The one required categorical field is bound
        // through the same ordered authority the composite uses.
        if (m_world.m_lighting and
            m_world.m_lighting->hasPublishedCells() and
            m_world.m_world_edge == WorldLightingPass::kWorldEdge) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, m_world.m_lighting->bindResolvedCells(cursor));
        } else {
            rhi::IBuffer *const resolved = m_test_resolved ? m_test_resolved.get() : m_lighting_fallback.get();
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_resolved_cells", resolved));
        }

        // The liquid response policy reads neither CH nor the BRDF LUT and no
        // thermal input; they stay bound for a declaration the linker kept (or a
        // future solid phase) through validity-guarded fields only. The solid
        // migration must bind the real committed CH through the lighting authority
        // rather than this fallback.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, bind_buffer("g_fine_ch", m_test_fine_ch ? m_test_fine_ch.get() : m_lighting_fallback.get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_terrain_visuals", m_world.m_terrain_palette.get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_source_materials", m_world.m_source_materials.get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_thermal_lut", m_thermal_lut.get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_view("g_brdf_lut", m_brdf_lut_view.get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_sampler("g_brdf_sampler", m_brdf_sampler.get()));

        rhi::IBuffer *const indices = m_world.m_diagnostic_atlas ? m_world.m_diagnostic_atlas.get()
                                                                 : m_world.m_cell_dummy.get();
        rhi::IBuffer *const colors = m_world.m_diagnostic_colors ? m_world.m_diagnostic_colors.get()
                                                                 : m_world.m_terrain_palette.get();
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_diagnostic_indices", indices));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_diagnostic_colors", colors));

        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_view("g_boundary_noise", m_boundary_noise_view.get()));
        rhi::IBuffer *const descriptors = m_world.m_texture_inventory and m_world.m_texture_inventory->m_cache
                                              ? m_world.m_texture_inventory->m_cache->descriptorBuffer()
                                              : nullptr;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_textures", descriptors));
        {
            const rhi::ShaderCursor sampler = cursor["g_sampler"];
            if (sampler.isValid()) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, sampler.setDescriptorHandle(m_terrain_sampler_handle));
            }
        }

        // The backdrop is a required liquid input (the refracted background copy
        // or the terrain fallback, never a null view).
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, bind_view(
                          "g_backdrop", m_background_ready ? m_background_slots[m_background_cursor].m_view.get()
                                                           : m_terrain_fallback_view.get()));

        // Fields only a non-folded dead branch could declare. Their content is
        // never read by these entries; the buffers exist so the root stays complete.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, bind_buffer("g_chunk_slots", m_chunk_slot_buffers[m_tile_ring_cursor].get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_contour_points", m_contour_points.get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_buffer("g_contour_cases", m_contour_cases.get()));
        const bool has_cells = m_world.m_visibility_has_cells or m_world.m_cell_atlas != nullptr;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            bind_buffer("g_cell_materials", has_cells ? m_world.m_cell_atlas.get() : m_world.m_cell_dummy.get()));
        if (cursor["g_particle_mask"].isValid() or cursor["g_particle_mask_sampler"].isValid()) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleMask_(device));
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_view("g_particle_mask", m_particle_mask_view.get()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_sampler("g_particle_mask_sampler", m_particle_mask_sampler.get()));

        // One authority for the ownership bit and the committed winner grid. It
        // binds the zero fallback and clears the bit when no current attempt owns
        // the grid, so the explicit shader test stays false.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindTerrainLighting_(cursor, availability));
        return {};
    }

    std::error_code GridPass::bindParticleSurfaceInputs_(
        rhi::IDevice &device, rhi::ShaderCursor &cursor, u32 &availability) {
        // The composite shades the reconstructed surface, so its root carries the
        // same validity-guarded response inputs the accumulation root does: the
        // palette that supplies the winning label's parameters, the categorical
        // publication it rechecks, the ownership authority and the backdrop it
        // samples. A program that folded one of them away is skipped.
        return bindParticleSurfaceSplat_(device, cursor, availability);
    }
}
