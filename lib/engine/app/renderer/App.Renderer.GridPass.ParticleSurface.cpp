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
    // screen-space particle density: GridPass-owned resources and passes
    // ------------------------------------------------------------------
    // One admitted liquid lane, a 1x additive (N, D[, O]) accumulation over the
    // same prepared rows/indirect arguments the direct classes use, and one
    // per-scene-sample composite at the lane's existing depth class slot. The
    // kernel and composite mathematics live in grid_particle_density.slang and
    // are consumed unchanged. There is no depth attachment in the accumulation,
    // no density history, no dirty-tile cache, no readback and no temporal
    // smoothing. Targets clear to exact zero every rendered frame.

    namespace {
        rhi::BufferRange particleSurfaceRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        // CPU mirror of grid_tiles.slang's ParticleSurfaceFrame. Field order,
        // widths and offsets are asserted where the program is created.
        struct ParticleSurfaceConstants {
            float m_pixel_world{};
            u32 m_material{0xffff'ffffu};
            u32 m_optical{};
            u32 m_pad{};
        };
        static_assert(sizeof(ParticleSurfaceConstants) == 16u and alignof(ParticleSurfaceConstants) == 4u);
        static_assert(PPR_OFFSETOF(ParticleSurfaceConstants, m_material) == 4u);
        static_assert(PPR_OFFSETOF(ParticleSurfaceConstants, m_optical) == 8u);

        [[nodiscard]] bool isLiquidVisual(const GridTerrainVisual &visual) noexcept {
            // The shader's own phase rule: liquid is authored liquid without the
            // solid bit. The depth class follows the authored optical-blend flag,
            // exactly as the packer's class nibble does.
            return (visual.m_surface_flags & kGridSurfaceParticleLiquid) != 0u and
                   (visual.m_surface_flags & kGridSurfaceParticleSolid) == 0u;
        }
    }

    // ------------------------------------------------------------------
    // admission and target lifetime
    // ------------------------------------------------------------------

    void GridPass::deriveParticleSurfaceLane_() noexcept {
        // The lane is a property of the admitted palette plus the publication,
        // never a user selector or a GPU readback. A world with no view, no
        // textured diagnostic lane, no liquid material or more than one liquid
        // material admits nothing: an ambiguous admission keeps every particle
        // on its existing direct class rather than mixing materials in one field.
        m_world.m_particle_surface = {};
        if (m_world.m_diagnostic_mode != EGridDiagnosticMode::textured or
            not m_world.m_gpu_particles or
            not m_world.m_gpu_particles->m_view.m_metadata) {
            return;
        }

        const auto &materials = m_world.m_gpu_particles->m_view.m_metadata->m_materials;
        u32 admitted = kParticleSurfaceNoMaterial;
        EGridParticleDrawClass draw_class = EGridParticleDrawClass::opaque;
        u32 liquid_materials = 0u;
        for (const u32 material: materials) {
            const auto visual = std::ranges::find(
                m_world.m_terrain_visuals, material, &GridTerrainVisual::m_material_id);
            if (visual == m_world.m_terrain_visuals.end() or not isLiquidVisual(*visual)) {
                continue;
            }

            ++liquid_materials;
            admitted = material;
            draw_class = (visual->m_surface_flags & kGridSurfaceOpticalBlend) != 0u ? EGridParticleDrawClass::optical
                                                                                    : EGridParticleDrawClass::opaque;
        }

        if (liquid_materials > 1u) {
            PPR_LOG(
                GridPass, debug, "particle density lane admission is ambiguous; keeping the direct classes",
                {
                    {"liquid_materials", liquid_materials},
                    {"publication_materials", materials.size()},
                });
            return;
        }

        if (liquid_materials == 0u) {
            return;
        }

        m_world.m_particle_surface = {admitted, draw_class};
    }

    std::error_code GridPass::ensureParticleSurfaceTarget_(
        rhi::IDevice &device, ParticleSurfaceSlot &slot, const u32 width, const u32 height) {
        const bool optical = m_world.m_particle_surface.m_draw_class == EGridParticleDrawClass::optical;
        if (slot.m_density and
            slot.m_width == width and
            slot.m_height == height and
            (slot.m_optical != nullptr) == optical) {
            return {};
        }

        // The caller proved this slot's last reader completed before selecting
        // it, so replacing the targets cannot race an in-flight composite or
        // accumulation.
        slot.m_density.setNull();
        slot.m_density_view.setNull();
        slot.m_optical.setNull();
        slot.m_optical_view.setNull();

        rhi::TextureDesc desc{};
        desc.type = rhi::TextureType::Texture2D;
        desc.size = {.width = width, .height = height, .depth = 1u};
        desc.arrayLength = 1u;
        desc.mipCount = 1u;
        desc.format = rhi::Format::RGBA16Float;
        desc.memoryType = rhi::MemoryType::DeviceLocal;
        desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "grid particle density (N, D)";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, slot.m_density.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, slot.m_density->getDefaultView(slot.m_density_view.writeRef()));

        // The optical numerator exists only for the optical lane: an opaque lane
        // needs no O target, no binding and no traffic.
        if (optical) {
            desc.format = rhi::Format::R16Float;
            desc.label = "grid particle density optical numerator";
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, slot.m_optical.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, slot.m_optical->getDefaultView(slot.m_optical_view.writeRef()));
        }

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
    // programs, sampler and signature-keyed pipelines
    // ------------------------------------------------------------------

    std::error_code GridPass::createParticleSurfacePrograms_(
        IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir) {
        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, service.loadModuleFromFile(
                          content_dir / TEXT("shaders") / TEXT("grid_tiles.slang"), "grid_tiles", module.writeRef()));

        // Reflect the lane constant buffer the splat vertex and the splat fragment
        // share. A target that widens a field or reorders the word fails program
        // creation here instead of misreading the lane material.
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
                 std::pair{"m_pixel_world", 0u}, {"m_material", 4u}, {"m_optical", 8u}}) {
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

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceSplat", "fragmentParticleSurfaceAccumulate",
                          m_particle_surface_accumulate_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceSplat", "fragmentParticleSurfaceAccumulateOptical",
                          m_particle_surface_accumulate_optical_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceComposite", "fragmentParticleSurfaceComposite",
                          m_particle_surface_composite_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceComposite", "fragmentParticleSurfaceCompositeOptical",
                          m_particle_surface_composite_optical_program));
        return {};
    }

    std::error_code GridPass::ensureParticleSurfaceSampler_(rhi::IDevice &device) {
        if (m_particle_surface_sampler) {
            return {};
        }

        // Bilinear reconstruction of the continuous 1x field with an exact-zero
        // border: a tap outside the target contributes nothing instead of
        // replicating the edge texel. One mip; no minification filter in use.
        rhi::SamplerDesc sampling{};
        sampling.minFilter = rhi::TextureFilteringMode::Linear;
        sampling.magFilter = rhi::TextureFilteringMode::Linear;
        sampling.mipFilter = rhi::TextureFilteringMode::Point;
        sampling.addressU = rhi::TextureAddressingMode::ClampToBorder;
        sampling.addressV = rhi::TextureAddressingMode::ClampToBorder;
        sampling.addressW = rhi::TextureAddressingMode::ClampToBorder;
        sampling.borderColor[0] = 0.0f;
        sampling.borderColor[1] = 0.0f;
        sampling.borderColor[2] = 0.0f;
        sampling.borderColor[3] = 0.0f;
        sampling.maxLOD = 0.0f;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createSampler(sampling, m_particle_surface_sampler.writeRef()));
        return {};
    }

    Expected<GridPass::GridCachedPipeline *> GridPass::particleSurfacePipeline_(
        rhi::IDevice &device, const RenderPipelineSignature &signature, rhi::IShaderProgram *const program,
        std::optional<RenderPipelineKey> &key, std::optional<GridCachedPipeline> &cache, const bool additive,
        const bool depth_test, const bool depth_write, const char *const label) {
        const auto formats = signature.colorFormats();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, program != nullptr and not formats.empty() and formats.size() <= 2u
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
            std::array<rhi::ColorTargetDesc, 2u> targets{};
            for (u32 index = 0u; index < formats.size(); ++index) {
                auto &target = targets[index];
                target.format = formats[index];
                target.enableBlend = true;
                if (additive) {
                    // One/One/Add on every component: the accumulation adds this
                    // fragment's (w*P, w[, w*alpha]) and never reads the target.
                    target.color = {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOp::Add};
                    target.alpha = target.color;
                } else {
                    // The composite blends premultiplied once: C*P over the scene.
                    target.color = {rhi::BlendFactor::One, rhi::BlendFactor::InvSrcAlpha, rhi::BlendOp::Add};
                    target.alpha = {rhi::BlendFactor::One, rhi::BlendFactor::InvSrcAlpha, rhi::BlendOp::Add};
                }
            }

            rhi::RenderPipelineDesc desc{};
            desc.program = program;
            desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            desc.targets = targets.data();
            desc.targetCount = static_cast<u32>(formats.size());
            desc.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);
            desc.depthStencil.format = signature.m_depth_stencil_format.value_or(rhi::Format::Undefined);
            desc.depthStencil.depthTestEnable = depth_test and signature.m_depth_stencil_format.has_value();
            desc.depthStencil.depthWriteEnable = depth_write and desc.depthStencil.depthTestEnable;
            desc.depthStencil.depthFunc = rhi::ComparisonFunc::LessEqual;
            desc.rasterizer.cullMode = rhi::CullMode::None;
            desc.label = label;

            GridCachedPipeline value;
            const auto result = device.createRenderPipeline(desc, value.m_pipeline.writeRef());
            const auto pipeline_error = rhi::make_error_code(result);
            if (pipeline_error) {
                const auto message = std::format(
                    "Grid particle-surface pipeline creation failed result={} category={} cause={} label={} "
                    "entries={} samples={} targets={} depth_format={} additive={} depth_test={} depth_write={}",
                    static_cast<int>(result), pipeline_error.category().name(), pipeline_error.message(), label,
                    program->getDesc().slangEntryPointCount, desc.multisample.sampleCount, desc.targetCount,
                    static_cast<int>(desc.depthStencil.format), additive, desc.depthStencil.depthTestEnable,
                    desc.depthStencil.depthWriteEnable);
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
    // pre-render accumulation
    // ------------------------------------------------------------------

    std::error_code GridPass::prepareParticleSurface(
        const PreRenderContext &context, const CameraSnapshot &camera, const rhi::Viewport &viewport,
        const rhi::ScissorRect &scissor) {
        // Composite admission is per frame: a lane that was not accumulated in
        // this submission is never read.
        m_particle_surface_ready = false;
        m_particle_surface_encoded = false;
        if (not m_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }

        const ParticleSurfaceLane lane = m_world.m_particle_surface;
        if (not lane.admitted()) {
            return {};
        }

        // Ensure the shared surface owners before the density pass is opened: a
        // retryable terrain-visual upload must not leave a render pass open, and the
        // pass must never depend on the scene pass having created an owner.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleSurfaceOwners_(context.m_device));

        // Full-raster targets, not the window or the final presentation size.
        const u32 width = scissor.maxX;
        const u32 height = scissor.maxY;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            width != 0u and height != 0u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        // Completion-fenced slot: never write a target a submitted frame may
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
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleSurfaceSampler_(context.m_device));

        m_particle_surface_pixel_world = particleSurfacePixelWorld_(camera);

        const bool optical = lane.m_draw_class == EGridParticleDrawClass::optical;
        // The accumulation pipeline carries an explicit 1-sample signature. The
        // pass never toggles the scene's spatial policy or selects an old x1
        // scene path.
        RenderPipelineSignature signature{};
        signature.m_color_formats[0] = rhi::Format::RGBA16Float;
        signature.m_color_format_count = 1u;
        if (optical) {
            signature.m_color_formats[1] = rhi::Format::R16Float;
            signature.m_color_format_count = 2u;
        }

        signature.m_sample_count = SampleCount::x1;

        auto &program = optical ? m_particle_surface_accumulate_optical_program : m_particle_surface_accumulate_program;
        auto &key = optical ? m_particle_surface_accumulate_optical_key : m_particle_surface_accumulate_key;
        auto &cache = optical ? m_particle_surface_accumulate_optical_pipeline : m_particle_surface_accumulate_pipeline;
        const auto pipeline = particleSurfacePipeline_(
            context.m_device, signature, program, key, cache, true, false, false,
            optical ? "grid particle density accumulation optical pipeline"
                    : "grid particle density accumulation pipeline");
        PPR_RETURN_ERROR_ON_FAIL(GridPass, pipeline);

        constexpr rhi::SubresourceRange whole_target{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        context.m_encoder.setTextureState(slot.m_density, whole_target, rhi::ResourceState::RenderTarget);
        if (optical) {
            context.m_encoder.setTextureState(slot.m_optical, whole_target, rhi::ResourceState::RenderTarget);
        }

        // Exact-zero clear every rendered frame; no partial clear, no history.
        std::array<rhi::RenderPassColorAttachment, 2u> attachments{};
        attachments[0].view = slot.m_density_view;
        attachments[0].loadOp = rhi::LoadOp::Clear;
        for (float &component: attachments[0].clearValue) {
            component = 0.0f;
        }

        if (optical) {
            attachments[1].view = slot.m_optical_view;
            attachments[1].loadOp = rhi::LoadOp::Clear;
            for (float &component: attachments[1].clearValue) {
                component = 0.0f;
            }
        }

        rhi::RenderPassDesc pass_desc{};
        pass_desc.colorAttachments = attachments.data();
        pass_desc.colorAttachmentCount = optical ? 2u : 1u;
        auto *const pass = context.m_encoder.beginRenderPass(pass_desc);
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));

        // The lanes keep their own scoped names: the density splat and the scene
        // composite are separate passes in separate callbacks.
        pass->setRenderState(
            {.viewports = {viewport},
                .viewportCount = 1u,
                .scissorRects = {scissor},
                .scissorRectCount = 1u,
                .vertexBuffers = {},
                .indexBuffer = {}});

        // A held but never-packed publication and a lane with no prepared rows
        // both clear and stop here: the composite then reads an exact-zero field
        // and discards. A retained pack re-rasterises from the current camera.
        const bool draw = m_world.m_gpu_particles and m_world.m_gpu_particles->m_prepared;
        if (draw) {
            auto &storage = *m_world.m_gpu_particles;
            auto &particle_slot = storage.m_slots[storage.m_selected_slot];
            auto *const root = (*pipeline)->m_slot_roots[selected].get();
            rhi::ShaderCursor cursor{root};
            // The backdrop is a shader binding input too: keep its view alive until
            // this submission completes, exactly as the direct particle lane does.
            auto &owners = (*pipeline)->m_slot_backdrop_owners[selected];
            const auto backdrop_view = m_background_ready ? m_background_slots[m_background_cursor].m_view
                                                          : m_terrain_fallback_view;
            if (std::ranges::none_of(owners, [&](const auto &owner) { return owner.get() == backdrop_view.get(); })) {
                owners.push_back(backdrop_view);
            }

            u32 availability{};
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bindParticleSurfaceSplat_(context.m_device, cursor, availability));
            owners.clear();
            owners.push_back(backdrop_view);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, uploadFrameConstants_((*pipeline)->m_slot_frame_cursors[selected], availability));
            if (cursor["g_grid_pass"].isValid()) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bindGridPass_(cursor, 0u, lane.m_draw_class));
            }

            ParticleSurfaceConstants constants{};
            constants.m_pixel_world = m_particle_surface_pixel_world;
            constants.m_material = lane.m_material;
            constants.m_optical = optical ? 1u : 0u;
            const rhi::ShaderCursor lane_field = cursor["g_particle_surface"];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                lane_field.isValid() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            rhi::ShaderCursor lane_cursor{};
            PPR_RETURN_ERROR_ON_FAIL(GridPass, lane_field.getDereferenced(lane_cursor));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, lane_cursor.setData(&constants, sizeof(constants)));

            // The prepared draw source is required: a hold presents its retained
            // rows, and an admitted lane without them is a caller error rather than
            // an empty draw.
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, storage.m_packed and particle_slot.m_ids and particle_slot.m_args
                              ? std::error_code{}
                              : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particles"].setBinding(
                              rhi::Binding(storage.m_packed, particleSurfaceRange(storage.m_packed))));
            rhi::IBuffer *const orientation = storage.m_orientation_draw.get();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                orientation != nullptr ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_orientation"].setBinding(
                              rhi::Binding(orientation, particleSurfaceRange(orientation))));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_ids"].setBinding(
                              rhi::Binding(particle_slot.m_ids, particleSurfaceRange(particle_slot.m_ids))));

            pass->bindPipeline((*pipeline)->m_pipeline, root);
            markPipelineUse_(**pipeline, selected);
            pass->drawIndirect(1u, rhi::BufferOffsetPair{particle_slot.m_args});
            ++m_draw_commands;
        }

        pass->end();
        if (draw and m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 2u);
        }

        context.m_encoder.setTextureState(slot.m_density, whole_target, rhi::ResourceState::ShaderResource);
        if (optical) {
            context.m_encoder.setTextureState(slot.m_optical, whole_target, rhi::ResourceState::ShaderResource);
        }

        m_particle_surface_encoded = true;
        m_particle_surface_ready = true;
        return {};
    }

    // ------------------------------------------------------------------
    // per-scene-sample composite
    // ------------------------------------------------------------------

    std::error_code GridPass::renderParticleSurface(
        const DrawContext &context, const EGridParticleDrawClass draw_class) {
        const ParticleSurfaceLane lane = m_world.m_particle_surface;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, lane.admitted() and m_particle_surface_ready and lane.m_draw_class == draw_class
                          ? std::error_code{}
                          : std::make_error_code(std::errc::operation_not_permitted));
        if (m_spatial_msaa) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSpatialContent());
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleSurfaceSampler_(context.m_device));

        const bool optical = draw_class == EGridParticleDrawClass::optical;
        // Composite state per class: both test LessEqual against the scene plane;
        // only the opaque class writes depth, and only where the coverage floor
        // let the fragment survive.
        const auto pipeline = particleSurfacePipeline_(
            context.m_device, context.m_render_pipeline_key,
            optical ? m_particle_surface_composite_optical_program.get() : m_particle_surface_composite_program.get(),
            optical ? m_particle_surface_composite_optical_key : m_particle_surface_composite_opaque_key,
            optical ? m_particle_surface_composite_optical_pipeline : m_particle_surface_composite_opaque_pipeline,
            false, true, not optical,
            optical ? "grid particle density optical composite pipeline" : "grid particle density composite pipeline");
        PPR_RETURN_ERROR_ON_FAIL(GridPass, pipeline);

        auto *const root = (*pipeline)->m_slot_roots[m_particle_surface_slot].get();
        rhi::ShaderCursor cursor{root};
        // The composite root carries only the categorical publication it rechecks
        // and the frame constants; the full material/source/thermal set is not
        // declared by this program and must not be bound.
        u32 availability{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindParticleSurfaceInputs_(context.m_device, cursor, availability));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, uploadFrameConstants_((*pipeline)->m_slot_frame_cursors[m_particle_surface_slot], availability));

        ParticleSurfaceConstants constants{};
        constants.m_pixel_world = m_particle_surface_pixel_world;
        constants.m_material = lane.m_material;
        constants.m_optical = optical ? 1u : 0u;
        rhi::ShaderCursor lane_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_surface"].getDereferenced(lane_cursor));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, lane_cursor.setData(&constants, sizeof(constants)));

        const auto &slot = m_particle_surface_slots[m_particle_surface_slot];
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, slot.m_density_view ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_density"].setBinding(rhi::Binding(slot.m_density_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_density_sampler"].setBinding(rhi::Binding(m_particle_surface_sampler)));
        if (optical) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                slot.m_optical_view ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_optical"].setBinding(rhi::Binding(slot.m_optical_view)));
        }

        context.m_pass.setRenderState(
            {.viewports = {context.m_viewport},
                .viewportCount = 1u,
                .scissorRects = {context.m_scissor},
                .scissorRectCount = 1u,
                .vertexBuffers = {},
                .indexBuffer = {}});
        context.m_pass.bindPipeline((*pipeline)->m_pipeline, root);
        markPipelineUse_(**pipeline, m_particle_surface_slot);
        // Full-raster triangle covering the scene plane; the per-sample fragment
        // does the threshold, normalization and premultiplied blend.
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
        // The diagnostic fallback the material set always declares; the density
        // pass never reads it, but a valid descriptor keeps the root complete.
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

        // The lane shades the liquid pre-edge surface, so the fine CH field is not
        // read. It may still be declared by a non-folded dead branch; a valid
        // fallback satisfies that declaration without a second authority call.
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

        // The backdrop is a required liquid input: the refracted background copy or
        // the terrain fallback, never a null view.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, bind_view(
                          "g_backdrop", m_background_ready ? m_background_slots[m_background_cursor].m_view.get()
                                                           : m_terrain_fallback_view.get()));

        // Fields only a non-folded dead branch could declare. Their content is
        // never read by this entry; the buffers exist so the root stays complete.
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
        // The composite reads the categorical publication, the ownership grid and
        // the palette; ensure their owners here so the draw never depends on the
        // particle or terrain draws having run first.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleSurfaceOwners_(device));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            m_world.m_terrain_palette ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));

        // The one categorical field the composite rechecks, from the same ordered
        // authority the material binder uses, narrowed to what this root declares.
        if (m_world.m_lighting and
            m_world.m_lighting->hasPublishedCells() and
            m_world.m_world_edge == WorldLightingPass::kWorldEdge) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, m_world.m_lighting->bindResolvedCells(cursor));
        } else if (m_test_resolved and m_test_fine_ch) {
            auto *const resolved = m_test_resolved.get();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                cursor["g_resolved_cells"].setBinding(rhi::Binding(resolved, particleSurfaceRange(resolved))));
        } else {
            auto *const fallback = m_lighting_fallback.get();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                cursor["g_resolved_cells"].setBinding(rhi::Binding(fallback, particleSurfaceRange(fallback))));
        }

        // One authority for the ownership bit and the committed winner grid. It
        // binds the zero fallback and clears the bit when no current attempt owns
        // the grid, so the explicit shader test stays false.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindTerrainLighting_(cursor, availability));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_terrain_visuals"].setBinding(
                          rhi::Binding(m_world.m_terrain_palette, particleSurfaceRange(m_world.m_terrain_palette))));
        return {};
    }
}
