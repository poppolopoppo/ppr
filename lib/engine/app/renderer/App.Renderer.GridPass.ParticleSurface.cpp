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
    //   presence marker  M = RGBA16Unorm nearest liquid centre (pixel offset from
    //                    the texel centre) + its material, Z = nearest pixel
    //                    distance. No blend, strict Less, depth writes, one
    //                    candidate quad of 1.7071 raster samples per admitted row.
    //   accumulation     Q = (sum w*base.rgb, sum w), G = (sum w*normal.xyz,
    //                    sum w*alpha). One/One/Add, no depth attachment.
    //   smoothing        two separable tiled label-masked Gaussian dispatches at
    //                    sigma = 0.15 world cell (+/-3 sigma), horizontal into a
    //                    scratch L/Q/G bundle and vertical into the final field.
    //   composite        one categorical four-tap reconstruction of the FILTERED
    //                    field, a binary occupancy decision per scene sample
    //                    (D > theta) and one deferred response evaluation per
    //                    occupied sample, drawn at each admitted class's existing
    //                    depth/blend slot. Coverage is C = max(B, F): the physical
    //                    body B wins every tie, and F is the unfiltered presence
    //                    marker evaluated at each scene sample.
    //
    // There is no temporal history, no dirty-tile cache and no readback. Every
    // target clears to its exact empty value every rendered frame, the marker
    // record is re-drawn from the current camera, and the two scratch bundles are
    // rewritten by the filter every rendered frame, so a held publication with an
    // unchanged camera reproduces the field and the marker bit for bit.

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

        // CPU mirror of grid_tiles.slang's ParticleSurfaceFilterFrame, the second
        // reflected contract of the surface lane. It carries this frame's own
        // zoom, not a fixed pixel width, so the smoothing is world-sized.
        struct ParticleSurfaceFilterConstants {
            float m_samples_per_cell{};
            float m_sigma_cell{};
            u32 m_halo{};
            u32 m_pad{};
            u32 m_extent_x{};
            u32 m_extent_y{};
            u32 m_pad2{};
            u32 m_pad3{};
        };
        static_assert(sizeof(ParticleSurfaceFilterConstants) == 32u and alignof(ParticleSurfaceFilterConstants) == 4u);
        static_assert(PPR_OFFSETOF(ParticleSurfaceFilterConstants, m_sigma_cell) == 4u);
        static_assert(PPR_OFFSETOF(ParticleSurfaceFilterConstants, m_halo) == 8u);
        static_assert(PPR_OFFSETOF(ParticleSurfaceFilterConstants, m_extent_x) == 16u);

        // grid_tiles.slang kParticleSurfaceSmoothingSigma and
        // kParticleSurfaceFilterMaxHalo. The sigma is the design's one admitted
        // smoothing scale in world native cells; the halo bound is what one 16x16
        // group's shared tile can load (18,432 bytes at the widest halo), so a
        // wider requirement is reported rather than silently truncated.
        constexpr float kParticleSurfaceSmoothingSigma{0.15f};
        constexpr u32 kParticleSurfaceFilterMaxHalo{8u};
        constexpr u32 kParticleSurfaceFilterTile{16u};

        // Raster samples per world native cell at the camera's drawing plane. The
        // smoothing sigma and its tap count are both derived from it, so the world
        // width of the filter is constant across the zoom range.
        float particleSurfaceSamplesPerCell(const float cell_size_m, const float pixel_world) noexcept {
            if (not (cell_size_m > 0.0f) or not (pixel_world > 0.0f)) {
                return 0.0f;
            }

            const float samples = cell_size_m / pixel_world;
            return std::isfinite(samples) ? samples : 0.0f;
        }

        // ceil(3*sigma*samples_per_cell), unclamped: the diagnostic and the
        // constant block both compare it against the shared tile's bound.
        u32 particleSurfaceRequiredHalo(const float samples_per_cell) noexcept {
            if (not (samples_per_cell > 0.0f)) {
                return 0u;
            }

            const float required = std::ceil(3.0f * kParticleSurfaceSmoothingSigma * samples_per_cell);
            if (not std::isfinite(required) or required <= 0.0f) {
                return 0u;
            }

            return static_cast<u32>(std::min(required, 4096.0f));
        }
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
        //
        // The previous admission is captured first so the diagnostic below can fire
        // only on a transition instead of on every preparation.
        const ParticleSurfaceLane previous = m_world.m_particle_surface;
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
        // Only the transition diagnostic reads this; the release log level
        // compiles PPR_LOG out, so the counter is otherwise unused.
        [[maybe_unused]] u32 admitted_materials{};
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
            ++admitted_materials;
        }

        if (not lane.admitted()) {
            return;
        }

        m_world.m_particle_surface = lane;
        if (lane != previous) {
            // Transition-only diagnostic: an unchanged admission is not re-logged
            // on every preparation.
            PPR_LOG(
                GridPass, debug, "particle surface lane admitted",
                {
                    {"liquid_materials", admitted_materials},
                    {"publication_materials", metadata->m_materials.size()},
                    {"class_mask", lane.m_class_mask},
                });
        }
    }

    std::error_code GridPass::ensureParticleSurfaceTarget_(
        rhi::IDevice &device, ParticleSurfaceSlot &slot, const u32 width, const u32 height) {
        // A complete bundle at the requested size is the only reusable state: a
        // partial creation failure must never look reusable. The caller proved
        // this slot's last reader completed before selecting it, so replacing the
        // bundle cannot race an in-flight composite or accumulation.
        const bool complete = slot.m_label and
                              slot.m_label_view and
                              slot.m_depth and
                              slot.m_depth_view and
                              slot.m_base and
                              slot.m_base_view and
                              slot.m_normal and
                              slot.m_normal_view and
                              slot.m_marker and
                              slot.m_marker_view and
                              slot.m_horizontal.complete() and
                              slot.m_vertical.complete();
        if (complete and slot.m_width == width and slot.m_height == height) {
            return {};
        }

        // Build into a local bundle and publish it only on complete success: a
        // recoverable failure part-way through leaves the previous (still fenced)
        // bundle and its dimensions untouched, so a later retry rebuilds from a
        // consistent state instead of matching cached dimensions to half a set.
        ParticleSurfaceSlot bundle{};

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
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, bundle.m_label.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bundle.m_label->getDefaultView(bundle.m_label_view.writeRef()));

        // Selection depth: the synthetic rho^2 distance, never scene depth and
        // never sampled at composite.
        desc.format = rhi::Format::D32Float;
        desc.usage = rhi::TextureUsage::DepthStencil;
        desc.defaultState = rhi::ResourceState::DepthWrite;
        desc.label = "grid particle surface selection depth";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, bundle.m_depth.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bundle.m_depth->getDefaultView(bundle.m_depth_view.writeRef()));

        // Q and G: the two attribute sums. RGBA16F carries signed normal
        // components and the existing additive blend contract.
        desc.format = rhi::Format::RGBA16Float;
        desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "grid particle surface base+density (Q)";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, bundle.m_base.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bundle.m_base->getDefaultView(bundle.m_base_view.writeRef()));

        desc.label = "grid particle surface normal+alpha (G)";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, bundle.m_normal.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bundle.m_normal->getDefaultView(bundle.m_normal_view.writeRef()));

        // The minimum-presence winner record, one texel per raster pixel. Eight
        // bytes per texel: .xy is the nearest centre's offset from the texel centre
        // over +/-4 pixels (unorm quantum 1.22e-4 px), .z is the winner's material
        // as m/65535 and .w is unused. Unorm rather than float because the offset
        // needs an absolute quantum; the composite evaluates the distance at its own
        // scene sample, so no coverage is ever stored here.
        desc.format = rhi::Format::RGBA16Unorm;
        desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "grid particle surface marker";
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, nullptr, bundle.m_marker.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bundle.m_marker->getDefaultView(bundle.m_marker_view.writeRef()));

        // The two separable smoothing bundles at the same raster size. Each is one
        // 4-byte label plus two RGBA16F sums per texel (20 bytes/texel, 40 bytes of
        // added scratch per slot); the horizontal pass writes the first and the
        // vertical pass reads it and writes the second, so neither is filtered in
        // place and no dispatch reads the buffer it writes.
        const auto create_bundle = [&](const char *const label, ParticleSurfaceBundle &into) -> std::error_code {
            rhi::TextureDesc scratch{};
            scratch.type = rhi::TextureType::Texture2D;
            scratch.size = {.width = width, .height = height, .depth = 1u};
            scratch.arrayLength = 1u;
            scratch.mipCount = 1u;
            scratch.memoryType = rhi::MemoryType::DeviceLocal;
            scratch.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
            scratch.defaultState = rhi::ResourceState::ShaderResource;
            scratch.label = label;

            scratch.format = rhi::Format::R32Uint;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(scratch, nullptr, into.m_label.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, into.m_label->getDefaultView(into.m_label_view.writeRef()));

            scratch.format = rhi::Format::RGBA16Float;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(scratch, nullptr, into.m_base.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, into.m_base->getDefaultView(into.m_base_view.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(scratch, nullptr, into.m_normal.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, into.m_normal->getDefaultView(into.m_normal_view.writeRef()));
            return {};
        };
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create_bundle("grid particle surface smoothing scratch (horizontal)", bundle.m_horizontal));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create_bundle("grid particle surface smoothing scratch (vertical)", bundle.m_vertical));

        // Publish: the old bundle (if any) is released here, after every new
        // member exists. The slot's completion value belongs to the slot, not to
        // the images, so it is preserved.
        bundle.m_width = width;
        bundle.m_height = height;
        bundle.m_last_use = slot.m_last_use;
        slot = std::move(bundle);
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

        // The smoothing constant block is the lane's second reflected contract: a
        // widened block or a reordered word must fail creation here instead of
        // misreading the zoom, the halo or the field extent.
        slang::TypeLayoutReflection *filter_frame{};
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_particle_filter") {
                filter_frame = parameter->getTypeLayout()->getElementTypeLayout();
                break;
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, filter_frame != nullptr and filter_frame->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) ==
                                                      sizeof(ParticleSurfaceFilterConstants)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 4u>{
                 std::pair{"m_samples_per_cell", 0u}, {"m_sigma_cell", 4u}, {"m_halo", 8u}, {"m_extent_x", 16u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < filter_frame->getFieldCount(); ++index) {
                auto *const field = filter_frame->getFieldByIndex(index);
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
        // SV_Depth and must not blend. The marker pass is its own program for the
        // same reason: a winner record plus a synthetic distance, no label, no
        // attribute sum. Accumulation and the two composites are separate programs
        // so each entry declares only the inputs it reads.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            create("vertexParticleSurfaceSplat", "fragmentParticleSurfaceSelect", m_particle_surface_select_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            create("vertexParticleSurfaceMarker", "fragmentParticleSurfaceMarker", m_particle_surface_marker_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceSplat", "fragmentParticleSurfaceAccumulate",
                          m_particle_surface_accumulate_program));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create(
                          "vertexParticleSurfaceComposite", "fragmentParticleSurfaceComposite",
                          m_particle_surface_composite_program));

        // The two separable smoothing passes. Each is its own compute program with
        // its own pipeline and one root per ring slot: a root's bindings are
        // authored once per frame, so the two dispatches cannot share one root even
        // though they run the same source template.
        const auto create_filter = [&](const char *const entry, ParticleSurfaceFilterProgram &into) -> std::error_code {
            shader::ComPtr<slang::IEntryPoint> point;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(entry, point.writeRef()));
            slang::IComponentType *entries[] = {point.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module.get();
            desc.slangEntryPoints = entries;
            desc.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createShaderProgram(desc, into.m_program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = into.m_program;
            pipeline.label = entry;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createComputePipeline(pipeline, into.m_pipeline.writeRef()));

            for (auto &root: into.m_roots) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createRootShaderObject(into.m_program, root.writeRef()));
            }

            return {};
        };
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create_filter("smoothParticleSurfaceHorizontal", m_particle_surface_filter_horizontal));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, create_filter("smoothParticleSurfaceVertical", m_particle_surface_filter_vertical));
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
                        // Winner record or integer label: no blend, full write
                        // mask. Depth is the synthetic selection distance.
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
            // first stable instance), and the marker pass shares that state: the
            // nearest centre wins, an exact tie keeps the first stable instance.
            // The composite shares the scene plane depth with the coplanar terrain
            // and particles (LessEqual).
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
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bindParticleSurfaceInputs_(context.m_device, cursor, availability));
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

        // --- minimum-presence marker -----------------------------------
        // One kParticleMarkerQuadRadius candidate per admitted liquid row, over the
        // same prepared instances and indirect arguments as the splat. The record is
        // the NEAREST particle centre per raster texel under the same
        // strict-Less/first-stable order the label uses, and it exists even where
        // every quadrature point of that row lies outside its texel: that is exactly
        // the far-zoom row the density field cannot express. Nothing here reads or
        // writes the label, the density or the filtered bundle.
        RenderPipelineSignature marker_signature{};
        marker_signature.m_color_formats[0] = rhi::Format::RGBA16Unorm;
        marker_signature.m_color_format_count = 1u;
        marker_signature.m_depth_stencil_format = rhi::Format::D32Float;
        marker_signature.m_sample_count = SampleCount::x1;
        const auto marker = particleSurfacePipeline_(
            context.m_device, marker_signature, m_particle_surface_marker_program.get(), m_particle_surface_marker_key,
            m_particle_surface_marker_pipeline, EParticleSurfacePipelineState::selection, true,
            "grid particle surface marker pipeline");
        PPR_RETURN_ERROR_ON_FAIL(GridPass, marker);

        state(slot.m_marker.get(), rhi::ResourceState::RenderTarget);
        state(slot.m_depth.get(), rhi::ResourceState::DepthWrite);

        rhi::RenderPassColorAttachment marker_attachment{};
        marker_attachment.view = slot.m_marker_view;
        marker_attachment.loadOp = rhi::LoadOp::Clear;
        marker_attachment.storeOp = rhi::StoreOp::Store;
        // The zero clear is the empty record: material zero owns nothing.
        rhi::RenderPassDepthStencilAttachment marker_depth{};
        marker_depth.view = slot.m_depth_view;
        marker_depth.depthLoadOp = rhi::LoadOp::Clear;
        // The marker distance is consumed only by this pass; the selection pass
        // clears the same synthetic buffer immediately afterwards.
        marker_depth.depthStoreOp = rhi::StoreOp::DontCare;
        marker_depth.depthClearValue = 1.0f;

        {
            rhi::RenderPassDesc pass_desc{};
            pass_desc.colorAttachments = &marker_attachment;
            pass_desc.colorAttachmentCount = 1u;
            pass_desc.depthStencilAttachment = &marker_depth;
            auto *const pass = context.m_encoder.beginRenderPass(pass_desc);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->pushDebugGroup("Particles.Surface.Marker", {});
            PPR_DEFER {
                pass->popDebugGroup();
                pass->end();
            };
            pass->setRenderState(
                {.viewports = {viewport},
                    .viewportCount = 1u,
                    .scissorRects = {scissor},
                    .scissorRectCount = 1u,
                    .vertexBuffers = {},
                    .indexBuffer = {}});

            rhi::ShaderCursor cursor{(*marker)->m_slot_roots[slot_index].get()};
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_common(**marker, cursor));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, bind_draw_source(cursor));
            pass->bindPipeline((*marker)->m_pipeline, (*marker)->m_slot_roots[slot_index].get());
            markPipelineUse_(**marker, slot_index);
            if (draw) {
                pass->drawIndirect(1u, rhi::BufferOffsetPair{args});
                ++m_draw_commands;
            }
        }
        if (draw and m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 7u);
        }

        // Both winner passes write the slot's synthetic distance buffer in separate
        // passes; order the two depth writes before the selection pass clears it and
        // publish the record to the composite's later read.
        context.m_encoder.globalBarrier();
        state(slot.m_marker.get(), rhi::ResourceState::ShaderResource);

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
        // Selection distance is consumed only by this pass: the next selection
        // clears it and neither accumulation nor composite samples it, so the
        // store is discarded. The attachment itself stays.
        depth_attachment.depthStoreOp = rhi::StoreOp::DontCare;
        depth_attachment.depthClearValue = 1.0f;

        {
            rhi::RenderPassDesc pass_desc{};
            pass_desc.colorAttachments = &label_attachment;
            pass_desc.colorAttachmentCount = 1u;
            pass_desc.depthStencilAttachment = &depth_attachment;
            auto *const pass = context.m_encoder.beginRenderPass(pass_desc);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->pushDebugGroup("Particles.Surface.Selection", {});
            PPR_DEFER {
                pass->popDebugGroup();
                pass->end();
            };
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
            pass->pushDebugGroup("Particles.Surface.Accumulation", {});
            PPR_DEFER {
                pass->popDebugGroup();
                pass->end();
            };
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
        }
        if (draw and m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 2u);
        }

        // Q/G are read by the smoothing pass; the label stays readable.
        state(slot.m_base.get(), rhi::ResourceState::ShaderResource);
        state(slot.m_normal.get(), rhi::ResourceState::ShaderResource);

        // --- smoothing: horizontal into scratch, vertical into the field ----
        // The field is filtered over its own target extent, not the frame's
        // scissor: the composite reconstructs every raster texel, and texels the
        // accumulation never visited hold the exact empty value, so filtering them
        // is a defined no-op rather than a truncated halo.
        ParticleSurfaceBundle accumulation_field{};
        accumulation_field.m_label = slot.m_label;
        accumulation_field.m_label_view = slot.m_label_view;
        accumulation_field.m_base = slot.m_base;
        accumulation_field.m_base_view = slot.m_base_view;
        accumulation_field.m_normal = slot.m_normal;
        accumulation_field.m_normal_view = slot.m_normal_view;
        {
            context.m_encoder.pushDebugGroup("Particles.Surface.Filter.Horizontal", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, encodeParticleSurfaceFilter_(
                              context, m_particle_surface_filter_horizontal, slot_index, accumulation_field,
                              slot.m_horizontal, slot.m_width, slot.m_height,
                              m_particle_surface_filter_samples_per_cell, m_particle_surface_filter_halo));
        }
        if (m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 5u);
        }

        // The horizontal scratch is complete before the vertical pass reads it.
        context.m_encoder.globalBarrier();
        {
            context.m_encoder.pushDebugGroup("Particles.Surface.Filter.Vertical", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, encodeParticleSurfaceFilter_(
                              context, m_particle_surface_filter_vertical, slot_index, slot.m_horizontal,
                              slot.m_vertical, slot.m_width, slot.m_height, m_particle_surface_filter_samples_per_cell,
                              m_particle_surface_filter_halo));
        }
        if (m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 6u);
        }

        context.m_encoder.globalBarrier();
        state(slot.m_vertical.m_label.get(), rhi::ResourceState::ShaderResource);
        state(slot.m_vertical.m_base.get(), rhi::ResourceState::ShaderResource);
        state(slot.m_vertical.m_normal.get(), rhi::ResourceState::ShaderResource);
        return {};
    }

    std::error_code GridPass::encodeParticleSurfaceFilter_(
        const PreRenderContext &context, ParticleSurfaceFilterProgram &program, const u32 slot_index,
        const ParticleSurfaceBundle &source, ParticleSurfaceBundle &target, const u32 width, const u32 height,
        const float samples_per_cell, const u32 halo) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, program.m_pipeline and
                              program.m_roots[slot_index] and
                              source.complete() and
                              target.complete() and
                              width != 0u and
                              height != 0u
                          ? std::error_code{}
                          : std::make_error_code(std::errc::state_not_recoverable));

        // One root per program and ring slot, authored here: the two dispatches
        // bind different bundles, so they cannot share a root.
        auto *const root = program.m_roots[slot_index].get();
        rhi::ShaderCursor cursor{root};
        u32 availability{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindParticleSurfaceInputs_(context.m_device, cursor, availability));
        rhi::ShaderCursor frame_cursor{};
        const rhi::ShaderCursor frame_field = cursor["g_grid_frame"];
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, frame_field.isValid() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, frame_field.getDereferenced(frame_cursor));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_(frame_cursor, availability));

        ParticleSurfaceFilterConstants constants{};
        constants.m_samples_per_cell = samples_per_cell;
        constants.m_sigma_cell = kParticleSurfaceSmoothingSigma;
        constants.m_halo = std::min(halo, kParticleSurfaceFilterMaxHalo);
        constants.m_extent_x = width;
        constants.m_extent_y = height;
        rhi::ShaderCursor filter_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_filter"].getDereferenced(filter_cursor));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, filter_cursor.setData(&constants, sizeof(constants)));

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_filter_source_label"].setBinding(rhi::Binding(source.m_label_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_filter_source_base"].setBinding(rhi::Binding(source.m_base_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_filter_source_normal"].setBinding(rhi::Binding(source.m_normal_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_filter_target_label"].setBinding(rhi::Binding(target.m_label_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_filter_target_base"].setBinding(rhi::Binding(target.m_base_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_filter_target_normal"].setBinding(rhi::Binding(target.m_normal_view)));

        constexpr rhi::SubresourceRange whole{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        context.m_encoder.setTextureState(source.m_label.get(), whole, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(source.m_base.get(), whole, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(source.m_normal.get(), whole, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(target.m_label.get(), whole, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setTextureState(target.m_base.get(), whole, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setTextureState(target.m_normal.get(), whole, rhi::ResourceState::UnorderedAccess);

        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        pass->bindPipeline(program.m_pipeline, root);
        pass->dispatchCompute(
            (width + kParticleSurfaceFilterTile - 1u) / kParticleSurfaceFilterTile,
            (height + kParticleSurfaceFilterTile - 1u) / kParticleSurfaceFilterTile, 1u);
        pass->end();

        ++m_dispatch_commands;
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

        // This frame's smoothing scale: sigma is a world width, so the tap count is
        // derived from the camera's own pixels-per-cell every frame. A truncated
        // requirement is reported on the transition (the release log level compiles
        // the record out, so the comparison, not the record, is what keeps this
        // state meaningful).
        m_particle_surface_filter_samples_per_cell = particleSurfaceSamplesPerCell(
            m_world.m_cell_size_m, m_particle_surface_pixel_world);
        const u32 required_halo = particleSurfaceRequiredHalo(m_particle_surface_filter_samples_per_cell);
        m_particle_surface_filter_halo = std::min(required_halo, kParticleSurfaceFilterMaxHalo);
        if (required_halo != m_particle_surface_filter_required_halo) [[unlikely]] {
            if (required_halo > kParticleSurfaceFilterMaxHalo) {
                PPR_LOG(
                    GridPass, warning, "particle surface smoothing truncated by the shared tile",
                    {
                        {"required_halo", required_halo},
                        {"loaded_halo", m_particle_surface_filter_halo},
                        {"samples_per_cell", m_particle_surface_filter_samples_per_cell},
                        {"sigma_samples", kParticleSurfaceSmoothingSigma * m_particle_surface_filter_samples_per_cell},
                    });
            }

            m_particle_surface_filter_required_halo = required_halo;
        }

        // Labelled selection, then same-label attributes, then the two separable
        // smoothing passes: the accumulation targets clear their exact empty value
        // every rendered frame and both scratch bundles are rewritten, so an empty
        // lane contributes nothing instead of last frame's field.
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

        // The composite reconstructs the SMOOTHED field: the vertical pass's
        // bundle, never the accumulation targets and never a scratch image.
        const auto &slot = m_particle_surface_slots[m_particle_surface_slot];
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            slot.m_vertical.complete() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_label"].setBinding(rhi::Binding(slot.m_vertical.m_label_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_base"].setBinding(rhi::Binding(slot.m_vertical.m_base_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_normal"].setBinding(rhi::Binding(slot.m_vertical.m_normal_view)));
        // The marker winner record the composite unions with the reconstructed body.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_marker"].setBinding(rhi::Binding(slot.m_marker_view)));

        context.m_pass.pushDebugGroup("Particles.Surface.Composite", {});
        PPR_DEFER {
            context.m_pass.popDebugGroup();
        };
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

    std::error_code GridPass::bindParticleSurfaceInputs_(
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
}
