module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.reconstruction_pass;
import :renderer.frame_fence;
import :renderer.presentation;
import :renderer.types;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Reconstruction, debug, none)

    // ------------------------------------------------------------------
    // CPU/GPU constant-buffer ABI
    // ------------------------------------------------------------------

    namespace {
        // Mirrors hable_temporal.slang. The unjittered current inverse
        // view-projection rebuilds the world-plane position for each resolved
        // pixel; the previous accepted unjittered view-projection maps that
        // position into the previous resolved grid. Mango vectors have a
        // user-provided empty default constructor, so every vector member is
        // zeroed explicitly rather than with `{}`; `prepare` assigns them all.
        struct alignas(16) ReconstructionParameters {
            float4x4 m_current_inverse_view_projection{identity_v};
            float4x4 m_previous_view_projection{identity_v};
            int2 m_resolved_extent{0, 0};
            int2 m_raster_extent{0, 0};
            int2 m_backing_extent{0, 0};
            int2 m_window_extent{0, 0};
            int2 m_previous_window_extent{0, 0};
            float2 m_jitter{0.0f, 0.0f};
            u32 m_pixel_scale{};
            u32 m_previous_pixel_scale{};
            u32 m_history_valid{};
            u32 m_reserved0{};
            int2 m_interior_origin{0, 0};
            int2 m_interior_extent{0, 0};
            int2 m_border_origin{0, 0};
            int2 m_border_extent{0, 0};
        };
        static_assert(sizeof(ReconstructionParameters) == 224u);
        static_assert(alignof(ReconstructionParameters) == 16u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_resolved_extent) == 128u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_previous_window_extent) == 160u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_jitter) == 168u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_pixel_scale) == 176u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_history_valid) == 184u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_interior_origin) == 192u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_interior_extent) == 200u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_border_origin) == 208u);
        static_assert(PPR_OFFSETOF(ReconstructionParameters, m_border_extent) == 216u);

        [[nodiscard]] rhi::SubresourceRange baseRange() noexcept {
            return {.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        }

        [[nodiscard]] slang::TypeLayoutReflection *parameterLayout(
            slang::IModule *const program, const char *const name) noexcept {
            auto *const layout = program->getLayout();
            if (layout == nullptr) {
                return nullptr;
            }

            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == name) {
                    return parameter->getTypeLayout()->getElementTypeLayout();
                }
            }

            return nullptr;
        }

        [[nodiscard]] bool hasFieldOffset(
            slang::TypeLayoutReflection *const layout, const char *const name, const u32 offset) noexcept {
            if (layout == nullptr) {
                return false;
            }

            for (unsigned index = 0u; index < layout->getFieldCount(); ++index) {
                auto *const field = layout->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    return field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                }
            }

            return false;
        }

        [[nodiscard]] std::error_code createProgram(
            rhi::IDevice &device, slang::IModule *const module, const char *const entry,
            rhi::ComPtr<rhi::IShaderProgram> &out) {
            shader::ComPtr<slang::IEntryPoint> entry_point;
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, module->findEntryPointByName(entry, entry_point.writeRef()));

            slang::IComponentType *components[] = {entry_point.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module;
            description.slangEntryPoints = components;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                Reconstruction, device.createShaderProgram(description, out.writeRef(), diagnostics.writeRef()));
            return {};
        }

        // Both entries include the same parameter block and bindings; verifying
        // the reflected layout once per program keeps the ABI check honest for
        // each compiled target.
        [[nodiscard]] std::error_code validateParameterLayout(slang::IModule *const module) {
            auto *const layout = parameterLayout(module, "g_reconstruction");
            const bool size_matches = layout != nullptr and layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) ==
                                                                sizeof(ReconstructionParameters);
            const bool fields_match =
                size_matches and
                hasFieldOffset(
                    layout, "m_resolved_extent", PPR_OFFSETOF(ReconstructionParameters, m_resolved_extent)) and
                hasFieldOffset(
                    layout, "m_previous_window_extent",
                    PPR_OFFSETOF(ReconstructionParameters, m_previous_window_extent)) and
                hasFieldOffset(layout, "m_jitter", PPR_OFFSETOF(ReconstructionParameters, m_jitter)) and
                hasFieldOffset(layout, "m_history_valid", PPR_OFFSETOF(ReconstructionParameters, m_history_valid)) and
                hasFieldOffset(
                    layout, "m_interior_origin", PPR_OFFSETOF(ReconstructionParameters, m_interior_origin)) and
                hasFieldOffset(
                    layout, "m_interior_extent", PPR_OFFSETOF(ReconstructionParameters, m_interior_extent)) and
                hasFieldOffset(layout, "m_border_origin", PPR_OFFSETOF(ReconstructionParameters, m_border_origin)) and
                hasFieldOffset(layout, "m_border_extent", PPR_OFFSETOF(ReconstructionParameters, m_border_extent));

            PPR_RETURN_ERROR_ON_FAIL(
                Reconstruction, fields_match ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            return {};
        }

        // Both kernels read one parameter block and one binding set; only the
        // root object differs.
        [[nodiscard]] std::error_code configureResolveRoot(
            rhi::IShaderObject &root, const ReconstructionParameters &parameters,
            const rhi::ComPtr<rhi::ITextureView> &scene, const rhi::ComPtr<rhi::ITextureView> &previous,
            const rhi::ComPtr<rhi::ITextureView> &next) {
            rhi::ShaderCursor cursor{&root};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, cursor["g_reconstruction"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, constants.setData(&parameters, sizeof(parameters)));
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, cursor["g_scene"].setBinding(rhi::Binding(scene)));
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, cursor["g_history_previous"].setBinding(rhi::Binding(previous)));
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, cursor["g_history_next"].setBinding(rhi::Binding(next)));
            return {};
        }
    }

    // ------------------------------------------------------------------
    // phase and padded scene camera
    // ------------------------------------------------------------------

    float2 ReconstructionPass::phaseJitter_(const u32 phase) noexcept {
        const u32 accepted = phase % kPhaseCount;
        return float2{0.5f * static_cast<float>(accepted & 1u), 0.5f * static_cast<float>((accepted >> 1u) & 1u)};
    }

    CameraSnapshot ReconstructionPass::makeSceneCamera_(
        const CameraSnapshot &camera, const PresentationGrid &grid, const float2 jitter) noexcept {
        // Padding extends right/bottom, so only the positive clip edges move.
        // The same signs as the engine's +Y-up clip and top-left pixel convention.
        const float ax = static_cast<float>(grid.m_window.x) /
                         static_cast<float>(grid.m_pixel_scale * static_cast<u32>(grid.m_backing.x));
        const float ay = static_cast<float>(grid.m_window.y) /
                         static_cast<float>(grid.m_pixel_scale * static_cast<u32>(grid.m_backing.y));

        float4x4 padding = float4x4::identity();
        padding[0][0] = ax;
        padding[1][1] = ay;
        padding[3][0] = ax - 1.0f;
        padding[3][1] = 1.0f - ay;

        // A positive raster jitter shifts the rendered image right/down.
        const float2 ndc_jitter{
            2.0f * jitter.x / static_cast<float>(grid.m_raster.x),
            -2.0f * jitter.y / static_cast<float>(grid.m_raster.y)};
        float4x4 jitter_matrix = float4x4::identity();
        jitter_matrix[3][0] = ndc_jitter.x;
        jitter_matrix[3][1] = ndc_jitter.y;

        CameraSnapshot scene = camera;
        scene.m_projection = camera.m_projection * padding;
        scene.m_jittered_projection = scene.m_projection * jitter_matrix;
        scene.m_view_projection = scene.m_view * scene.m_projection;
        scene.m_jittered_view_projection = scene.m_view * scene.m_jittered_projection;
        scene.m_invert_projection = inverse(scene.m_projection);
        scene.m_invert_view_projection = inverse(scene.m_view_projection);
        scene.m_invert_jittered_view_projection = inverse(scene.m_jittered_view_projection);
        scene.m_jitter = ndc_jitter;
        scene.m_viewport_size = float2{static_cast<float>(grid.m_raster.x), static_cast<float>(grid.m_raster.y)};

        scene.m_frustum = makeZeroToOneFrustum(scene.m_view_projection);
        const float4x4 zero_to_one_vp = scene.m_view_projection * float4x4{
                                                                      float4{1.0f, 0.0f, 0.0f, 0.0f},
                                                                      float4{0.0f, 1.0f, 0.0f, 0.0f},
                                                                      float4{0.0f, 0.0f, 2.0f, 0.0f},
                                                                      float4{0.0f, 0.0f, -1.0f, 1.0f},
                                                                  };
        scene.m_ray_frustum = RayFrustum(zero_to_one_vp);
        return scene;
    }

    // ------------------------------------------------------------------
    // interior dispatch region
    // ------------------------------------------------------------------
    // A block references only the source pixels within one lattice pixel of its
    // floor-divided origin, at any of the four sub-pixel sample positions. The
    // reconstruction tests re-derive that reach, and the fallback reachability
    // of the remaining pixels, from the independent lattice enumeration.

    namespace {
        constexpr i32 kLatticeReach{1};
        constexpr std::array<float, 4> kAxisSampleOffsets{1.0f / 8.0f, 3.0f / 8.0f, 5.0f / 8.0f, 7.0f / 8.0f};

        // Mirrors the shader's sourceSampleValid for one axis: an origin is
        // interior only when its whole neighbourhood is inside the raster and
        // every sample position lands inside the resolved extent.
        [[nodiscard]] bool latticeAxisReferenceValid(
            const i32 origin, const i32 raster, const i32 resolved, const float jitter) noexcept {
            for (i32 reach = -kLatticeReach; reach <= kLatticeReach; ++reach) {
                const i32 pixel = origin + reach;
                if (pixel < 0 or pixel >= raster) {
                    return false;
                }

                for (const float offset: kAxisSampleOffsets) {
                    const float stationary = 2.0f * (static_cast<float>(pixel) + offset - jitter);
                    if (stationary < 0.0f or stationary >= static_cast<float>(resolved)) {
                        return false;
                    }
                }
            }

            return true;
        }

        // The valid origins form one interval: the lower and upper constraints
        // are monotone, so the first admissible origin and the last describe
        // every admissible one. Both scans stop within a few steps for a legal
        // grid; only a grid with no interior at all inspects the full axis.
        [[nodiscard]] std::optional<std::pair<i32, i32>> latticeAxisInterior(
            const i32 raster, const i32 resolved, const float jitter) noexcept {
            std::optional<i32> low;
            for (i32 origin = 0; origin <= raster and not low.has_value(); ++origin) {
                if (latticeAxisReferenceValid(origin, raster, resolved, jitter)) {
                    low = origin;
                }
            }
            if (not low.has_value()) {
                return std::nullopt;
            }

            std::optional<i32> high;
            for (i32 origin = raster - 1; origin >= *low and not high.has_value(); --origin) {
                if (latticeAxisReferenceValid(origin, raster, resolved, jitter)) {
                    high = origin;
                }
            }
            if (not high.has_value()) {
                return std::nullopt;
            }

            return std::pair{*low, *high};
        }
    }

    ReconstructionPass::InteriorRegion ReconstructionPass::interiorRegion(
        const PresentationGrid &grid, const float2 jitter) noexcept {
        const auto x = latticeAxisInterior(grid.m_raster.x, grid.m_resolved.x, jitter.x);
        const auto y = latticeAxisInterior(grid.m_raster.y, grid.m_resolved.y, jitter.y);
        if (not x.has_value() or not y.has_value()) {
            return {};
        }

        // A block origin maps to resolved pixels 2*o - shift/2 through
        // 2*o + 1 - shift/2, where shift = int(4*jitter) is the integral phase
        // displacement the shader's lattice base applies.
        const i32 shift_x = static_cast<i32>(4.0f * jitter.x) / 2;
        const i32 shift_y = static_cast<i32>(4.0f * jitter.y) / 2;
        const i32 low_x = std::max(0, 2 * x->first - shift_x);
        const i32 low_y = std::max(0, 2 * y->first - shift_y);
        const i32 high_x = std::min(grid.m_backing.x - 1, 2 * x->second + 1 - shift_x);
        const i32 high_y = std::min(grid.m_backing.y - 1, 2 * y->second + 1 - shift_y);
        if (low_x > high_x or low_y > high_y) {
            return {};
        }

        return InteriorRegion{
            .m_origin = int2{low_x, low_y},
            .m_extent = int2{high_x - low_x + 1, high_y - low_y + 1},
        };
    }

    // ------------------------------------------------------------------
    // border dispatch partition
    // ------------------------------------------------------------------

    std::array<ReconstructionPass::InteriorRegion, ReconstructionPass::kBorderRegionCount>
    ReconstructionPass::borderRegions(const int2 backing, const InteriorRegion &interior) noexcept {
        std::array<InteriorRegion, kBorderRegionCount> regions{};
        if (interior.empty()) {
            regions[0] = InteriorRegion{.m_origin = int2{0, 0}, .m_extent = backing};
            return regions;
        }

        const int2 low = interior.m_origin;
        const int2 high{low.x + interior.m_extent.x, low.y + interior.m_extent.y};

        // Full-width top/bottom own the corners. Side bands only cover the
        // interior's Y interval, so all four half-open rectangles are disjoint.
        regions[0] = InteriorRegion{.m_origin = int2{0, 0}, .m_extent = int2{backing.x, low.y}};
        regions[1] = InteriorRegion{.m_origin = int2{0, high.y}, .m_extent = int2{backing.x, backing.y - high.y}};
        regions[2] = InteriorRegion{.m_origin = int2{0, low.y}, .m_extent = int2{low.x, interior.m_extent.y}};
        regions[3] = InteriorRegion{
            .m_origin = int2{high.x, low.y}, .m_extent = int2{backing.x - high.x, interior.m_extent.y}};

        for (auto &region: regions) {
            if (region.empty()) {
                region = InteriorRegion{};
            }
        }

        return regions;
    }

    // ------------------------------------------------------------------
    // initialization and teardown
    // ------------------------------------------------------------------

    std::error_code ReconstructionPass::initialize(
        IShaderService &shader_service, const std::filesystem::path &content_dir) {
        const safe_ptr<IRhiService> rhi_service = IRhiService::get();
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction,
            rhi_service != nullptr ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        rhi::IDevice &device = rhi_service->getDevice();

        bool initialization_complete = false;
        PPR_DEFER {
            if (not initialization_complete) {
                releaseGpuResources_();
            }
        };

        // Requirements are explicit: RGBA16F render/multisample/load/UAV and
        // the standard 4x pattern. No alternate AA path exists.
        rhi::FormatSupport support{};
        PPR_RETURN_ERROR_ON_FAIL(Reconstruction, device.getFormatSupport(rhi::Format::RGBA16Float, &support));
        constexpr u32 required = static_cast<u32>(rhi::FormatSupport::Texture) |
                                 static_cast<u32>(rhi::FormatSupport::RenderTarget) |
                                 static_cast<u32>(rhi::FormatSupport::Multisampling) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderLoad) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderUavStore);
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, (static_cast<u32>(support) & required) == required
                                ? std::error_code{}
                                : std::make_error_code(std::errc::function_not_supported));
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, device.hasFeature("standard-sample-locations-4")
                                ? std::error_code{}
                                : std::make_error_code(std::errc::function_not_supported));

        PPR_RETURN_ERROR_ON_FAIL(Reconstruction, createPrograms_(shader_service, device, content_dir));

        m_ready = true;
        initialization_complete = true;

        PPR_LOG(Reconstruction, info, "ReconstructionPass initialized");
        return {};
    }

    std::error_code ReconstructionPass::createPrograms_(
        IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir) {
        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, shader_service.loadModuleFromFile(
                                content_dir / "shaders" / "hable_temporal.slang", "hable_temporal", module.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(Reconstruction, validateParameterLayout(module.get()));

        // Both entries share the module's parameter block and bindings, but each
        // gets its own program and pipeline so neither kernel inherits the
        // other's register allocation.
        const auto create_kernel = [&device, module = module.get()](
                                       const char *const entry, const char *const label, const u32 regions,
                                       ResolveKernel &kernel) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, createProgram(device, module, entry, kernel.m_program));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = kernel.m_program;
            pipeline.label = label;
            PPR_RETURN_ERROR_ON_FAIL(
                Reconstruction, device.createComputePipeline(pipeline, kernel.m_pipeline.writeRef()));

            kernel.m_roots.resize(kFrameRingSlots * regions);
            for (auto &root: kernel.m_roots) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Reconstruction, device.createRootShaderObject(kernel.m_program, root.writeRef()));
            }

            return {};
        };

        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction,
            create_kernel("reconstructTemporalInterior", "Hable reconstruction interior", 1u, m_interior));
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction,
            create_kernel("reconstructTemporalBorder", "Hable reconstruction border", kBorderRegionCount, m_border));
        return {};
    }

    bool ReconstructionPass::matchesExtents_(
        const ReconstructionPass::FrameResources &resources, const PresentationGrid &grid) noexcept {
        return resources.m_scene and
               resources.m_scene_extent.x == grid.m_raster.x and
               resources.m_scene_extent.y == grid.m_raster.y and
               resources.m_history[0] and
               resources.m_backing_extent.x == grid.m_backing.x and
               resources.m_backing_extent.y == grid.m_backing.y;
    }

    Expected<ReconstructionPass::FrameResources> ReconstructionPass::createResources_(
        rhi::IDevice &device, const PresentationGrid &grid) const {
        // Allocate into locals; the caller publishes the extents only after
        // every object exists, so a partial failure cannot leave a wrong-sized
        // or null view under the old extent.
        ReconstructionPass::FrameResources resources{};

        rhi::TextureDesc scene_description{};
        scene_description.type = rhi::TextureType::Texture2DMS;
        scene_description.size = {
            .width = static_cast<u32>(grid.m_raster.x), .height = static_cast<u32>(grid.m_raster.y), .depth = 1u};
        scene_description.arrayLength = 1u;
        scene_description.mipCount = 1u;
        scene_description.format = rhi::Format::RGBA16Float;
        scene_description.sampleCount = kSampleCount;
        scene_description.memoryType = rhi::MemoryType::DeviceLocal;
        scene_description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        scene_description.defaultState = rhi::ResourceState::RenderTarget;
        scene_description.label = "reconstruction scene color";
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Reconstruction, device.createTexture(scene_description, nullptr, resources.m_scene.writeRef()));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Reconstruction, resources.m_scene->getDefaultView(resources.m_scene_view.writeRef()));

        for (u32 index = 0u; index < resources.m_history.size(); ++index) {
            rhi::TextureDesc history_description{};
            history_description.type = rhi::TextureType::Texture2D;
            history_description.size = {
                .width = static_cast<u32>(grid.m_backing.x), .height = static_cast<u32>(grid.m_backing.y), .depth = 1u};
            history_description.arrayLength = 1u;
            history_description.mipCount = 1u;
            history_description.format = rhi::Format::RGBA16Float;
            history_description.sampleCount = 1u;
            history_description.memoryType = rhi::MemoryType::DeviceLocal;
            // Copy flags support diagnostic readback/padding-poisoning seams;
            // the production pass only needs SRV/UAV.
            history_description.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                static_cast<u32>(rhi::TextureUsage::UnorderedAccess) |
                static_cast<u32>(rhi::TextureUsage::CopySource) |
                static_cast<u32>(rhi::TextureUsage::CopyDestination));
            history_description.defaultState = rhi::ResourceState::ShaderResource;
            history_description.label = "reconstruction resolved history";
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Reconstruction,
                device.createTexture(history_description, nullptr, resources.m_history[index].writeRef()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Reconstruction,
                resources.m_history[index]->getDefaultView(resources.m_history_views[index].writeRef()));
        }

        resources.m_scene_extent = grid.m_raster;
        resources.m_backing_extent = grid.m_backing;
        return resources;
    }

    void ReconstructionPass::deferResources_(ReconstructionPass::FrameResources &&resources) noexcept {
        if (resources.m_scene) {
            m_deferred_releases.defer(
                resources.m_scene_use, [held = std::move(resources.m_scene)]() mutable { held.setNull(); });
        }
        if (resources.m_scene_view) {
            m_deferred_releases.defer(
                resources.m_scene_use, [held = std::move(resources.m_scene_view)]() mutable { held.setNull(); });
        }
        for (u32 index = 0u; index < resources.m_history.size(); ++index) {
            if (resources.m_history[index]) {
                m_deferred_releases.defer(
                    resources.m_history_use,
                    [held = std::move(resources.m_history[index])]() mutable { held.setNull(); });
            }
            if (resources.m_history_views[index]) {
                m_deferred_releases.defer(
                    resources.m_history_use,
                    [held = std::move(resources.m_history_views[index])]() mutable { held.setNull(); });
            }
        }
    }

    ReconstructionPass::FrameResources &ReconstructionPass::pendingResources_() noexcept {
        return m_pending.m_tentative ? *m_tentative : m_committed;
    }

    const ReconstructionPass::FrameResources &ReconstructionPass::pendingResources_() const noexcept {
        return m_pending.m_tentative ? *m_tentative : m_committed;
    }

    std::error_code ReconstructionPass::shutdown() {
        PPR_LOG(Reconstruction, info, "ReconstructionPass shut down");
        releaseGpuResources_();
        return {};
    }

    std::error_code ReconstructionPass::notifyDeviceLost() noexcept {
        // GPU objects are gone: drain deferred releases and drop every owned
        // resource without waiting.
        releaseGpuResources_();
        return {};
    }

    void ReconstructionPass::releaseGpuResources_() noexcept {
        std::ignore = m_deferred_releases.drainAll();
        m_committed = FrameResources{};
        m_tentative.reset();
        m_interior = ResolveKernel{};
        m_border = ResolveKernel{};
        m_root_use = {};

        m_history_grid = {};
        m_history_camera = CameraSnapshot{};
        m_history_world = {};
        m_history_camera_cut = 0u;
        m_history_index = 0u;
        m_phase = 0u;
        m_history_established = false;
        m_root_cursor = 0u;
        m_pending = PendingFrame{};
        m_ready = false;
    }

    // ------------------------------------------------------------------
    // frame entry and resource proposal
    // ------------------------------------------------------------------

    std::error_code ReconstructionPass::beginFrame(
        rhi::IDevice &device, const CameraSnapshot &camera, const PresentationGrid &grid, const GridWorldIdentity world,
        const u64 camera_cut, const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        // The caller supplies the checked W/P/R/S/B tuple; recomputing it keeps
        // every derived extent and the padding arithmetic verified here.
        const auto expected = makePresentationGrid(grid.m_window, grid.m_pixel_scale);
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction,
            expected.has_value() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, *expected == grid ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction,
            m_pending.m_active ? std::make_error_code(std::errc::operation_not_permitted) : std::error_code{});

        // Root shader objects are frame resources too: never bind over one
        // whose previous stage-B submission may still be reading it. Admission
        // runs before any resource proposal so a busy frame changes nothing.
        const u32 root = selectRingSlot(completed, m_root_use, m_root_cursor);
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, slotFenceComplete(completed, m_root_use[root])
                                ? std::error_code{}
                                : std::make_error_code(std::errc::device_or_resource_busy));

        // A tentative pair is used only when the requested extents differ from
        // the committed pair. It never replaces the committed pair until
        // notifySubmitted; a discard retires only the tentative pair.
        PPR_ASSERT(not m_tentative);
        if (not matchesExtents_(m_committed, grid)) {
            auto created = createResources_(device, grid);
            PPR_RETURN_ERROR_ON_FAIL(Reconstruction, created.has_value() ? std::error_code{} : created.error());
            m_tentative = std::move(*created);
        }

        const bool reset = not m_history_established or
                           m_history_world != world or
                           m_history_camera_cut != camera_cut or
                           not (m_history_grid == grid);
        m_last_begin_reset = reset;
        const u32 phase = reset ? 0u : (m_phase + 1u) % kPhaseCount;
        // With a reused committed pair, always write the non-committed member
        // so even a reset attempt cannot overwrite the accepted history bytes
        // before notifySubmitted. A tentative pair starts at zero.
        const u32 write_index = m_tentative.has_value() ? 0u : 1u - m_history_index;

        m_pending = PendingFrame{};
        m_pending.m_camera = CameraSnapshot{camera};
        m_pending.m_scene_camera = makeSceneCamera_(camera, grid, phaseJitter_(phase));
        m_pending.m_grid = grid;
        m_pending.m_world = world;
        m_pending.m_camera_cut = camera_cut;
        m_pending.m_phase = phase;
        m_pending.m_root = root;
        m_pending.m_write_index = write_index;
        m_pending.m_previous_index = 1u - write_index;
        m_pending.m_history_valid = not reset;
        m_pending.m_tentative = m_tentative.has_value();
        m_pending.m_active = true;

        m_root_cursor = (root + 1u) % kFrameRingSlots;
        return {};
    }

    const CameraSnapshot &ReconstructionPass::sceneCamera() const noexcept {
        PPR_ASSERT(m_pending.m_active);
        return m_pending.m_scene_camera;
    }

    rhi::ITexture &ReconstructionPass::sceneTexture() const noexcept {
        PPR_ASSERT(m_pending.m_active);
        const FrameResources &resources = pendingResources_();
        PPR_ASSERT(resources.m_scene);
        return *resources.m_scene;
    }

    // ------------------------------------------------------------------
    // reconstruction encoding and resolved binding
    // ------------------------------------------------------------------

    std::error_code ReconstructionPass::encodeDispatch_(
        const ResolveKernel &kernel, const u32 root, const int2 extent, const PreRenderContext &context) {
        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        pass->bindPipeline(kernel.m_pipeline, kernel.m_roots[root]);
        pass->dispatchCompute((static_cast<u32>(extent.x) + 7u) / 8u, (static_cast<u32>(extent.y) + 7u) / 8u, 1u);
        pass->end();
        return {};
    }

    std::error_code ReconstructionPass::prepare(const PreRenderContext &context) {
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, m_pending.m_active and not m_pending.m_prepared
                                ? std::error_code{}
                                : std::make_error_code(std::errc::operation_not_permitted));

        const PendingFrame &pending = m_pending;
        const FrameResources &resources = pendingResources_();
        rhi::ITexture *const scene = resources.m_scene.get();
        rhi::ITexture *const previous = resources.m_history[pending.m_previous_index].get();
        rhi::ITexture *const next = resources.m_history[pending.m_write_index].get();
        const rhi::ComPtr<rhi::ITextureView> &scene_view = resources.m_scene_view;
        const rhi::ComPtr<rhi::ITextureView> &previous_view = resources.m_history_views[pending.m_previous_index];
        const rhi::ComPtr<rhi::ITextureView> &next_view = resources.m_history_views[pending.m_write_index];

        const float2 jitter = phaseJitter_(pending.m_phase);
        ReconstructionParameters parameters{};
        parameters.m_current_inverse_view_projection = pending.m_camera.m_invert_view_projection;
        parameters.m_previous_view_projection = pending.m_history_valid ? m_history_camera.m_view_projection
                                                                        : pending.m_camera.m_view_projection;
        parameters.m_resolved_extent = pending.m_grid.m_resolved;
        parameters.m_raster_extent = pending.m_grid.m_raster;
        parameters.m_backing_extent = pending.m_grid.m_backing;
        parameters.m_window_extent = pending.m_grid.m_window;
        parameters.m_previous_window_extent = pending.m_history_valid ? m_history_grid.m_window
                                                                      : pending.m_grid.m_window;
        parameters.m_jitter = jitter;
        parameters.m_pixel_scale = pending.m_grid.m_pixel_scale;
        parameters.m_previous_pixel_scale = pending.m_history_valid ? m_history_grid.m_pixel_scale
                                                                    : pending.m_grid.m_pixel_scale;
        parameters.m_history_valid = pending.m_history_valid ? 1u : 0u;
        const InteriorRegion interior = interiorRegion(pending.m_grid, jitter);
        parameters.m_interior_origin = interior.m_origin;
        parameters.m_interior_extent = interior.m_extent;

        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, configureResolveRoot(
                                *m_interior.m_roots[pending.m_root], parameters, scene_view, previous_view, next_view));

        const auto borders = borderRegions(pending.m_grid.m_backing, interior);
        for (u32 index = 0u; index < borders.size(); ++index) {
            if (borders[index].empty()) {
                continue;
            }

            parameters.m_border_origin = borders[index].m_origin;
            parameters.m_border_extent = borders[index].m_extent;
            const u32 root = pending.m_root * kBorderRegionCount + index;
            PPR_RETURN_ERROR_ON_FAIL(
                Reconstruction,
                configureResolveRoot(*m_border.m_roots[root], parameters, scene_view, previous_view, next_view));
        }

        const rhi::SubresourceRange range = baseRange();
        context.m_encoder.setTextureState(scene, range, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(previous, range, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(next, range, rhi::ResourceState::UnorderedAccess);

        // The interior rectangle and the rest of the backing are disjoint and
        // together cover every backing pixel, so each pixel is written once.
        const hal::ProfileScope scope{"Hable.Resolve"};
        context.m_encoder.pushDebugGroup("Hable.Resolve", {});
        PPR_DEFER {
            context.m_encoder.popDebugGroup();
        };

        if (not interior.empty()) {
            const hal::ProfileScope interior_scope{"Hable.Resolve.Interior"};
            context.m_encoder.pushDebugGroup("Hable.Resolve.Interior", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(
                Reconstruction, encodeDispatch_(m_interior, pending.m_root, interior.m_extent, context));
        }

        {
            const hal::ProfileScope border_scope{"Hable.Resolve.Border"};
            context.m_encoder.pushDebugGroup("Hable.Resolve.Border", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            for (u32 index = 0u; index < borders.size(); ++index) {
                if (borders[index].empty()) {
                    continue;
                }

                const u32 root = pending.m_root * kBorderRegionCount + index;
                PPR_RETURN_ERROR_ON_FAIL(
                    Reconstruction, encodeDispatch_(m_border, root, borders[index].m_extent, context));
            }
        }

        // Post-processing in the same stage-B submission reads the written
        // history as a shader resource.
        context.m_encoder.setTextureState(next, range, rhi::ResourceState::ShaderResource);
        m_pending.m_prepared = true;
        return {};
    }

    ResolvedHdrView ReconstructionPass::resolved() const {
        PPR_ASSERT(m_pending.m_active and m_pending.m_prepared);
        const FrameResources &resources = pendingResources_();
        ResolvedHdrView view{};
        view.m_view = resources.m_history_views[m_pending.m_write_index];
        view.m_world = m_pending.m_world;
        view.m_backing = m_pending.m_grid.m_backing;
        view.m_valid = m_pending.m_grid.m_resolved;
        return view;
    }

    std::error_code ReconstructionPass::finalize(const PreRenderContext &context) {
        PPR_RETURN_ERROR_ON_FAIL(
            Reconstruction, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        if (not m_pending.m_active or not m_pending.m_prepared) {
            return {};
        }

        const FrameResources &resources = pendingResources_();
        const rhi::SubresourceRange range = baseRange();
        context.m_encoder.setTextureState(
            resources.m_history[m_pending.m_write_index], range, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(resources.m_scene, range, rhi::ResourceState::RenderTarget);
        return {};
    }

    // ------------------------------------------------------------------
    // settlement
    // ------------------------------------------------------------------

    void ReconstructionPass::notifySceneSubmitted(const FenceCompletionValue submit_value) noexcept {
        if (not m_pending.m_active) {
            return;
        }

        FrameResources &resources = pendingResources_();
        resources.m_scene_use = std::max(resources.m_scene_use, submit_value);
    }

    void ReconstructionPass::notifySubmitted(const FenceCompletionValue submit_value) noexcept {
        if (not m_pending.m_active or not m_pending.m_prepared) {
            return;
        }

        FrameResources &resources = pendingResources_();
        resources.m_scene_use = std::max(resources.m_scene_use, submit_value);
        resources.m_history_use = std::max(resources.m_history_use, submit_value);
        m_root_use[m_pending.m_root] = std::max(m_root_use[m_pending.m_root], submit_value);

        if (m_pending.m_tentative) {
            // Acceptance swaps the tentative pair in and retires the old
            // committed pair only after its readers complete.
            deferResources_(std::move(m_committed));
            m_committed = std::move(*m_tentative);
            m_tentative.reset();
        }

        m_history_established = true;
        m_history_index = m_pending.m_write_index;
        m_phase = m_pending.m_phase;
        m_history_world = m_pending.m_world;
        m_history_grid = m_pending.m_grid;
        m_history_camera = m_pending.m_camera;
        m_history_camera_cut = m_pending.m_camera_cut;
        m_pending = PendingFrame{};
    }

    void ReconstructionPass::discardUnsubmitted() noexcept {
        // Definitely unsubmitted work commits nothing. A tentative allocation
        // is retired after its reported stage-A readers; the committed pair and
        // all accepted history state remain valid.
        if (m_pending.m_tentative and m_tentative) {
            deferResources_(std::move(*m_tentative));
            m_tentative.reset();
        }
        m_pending = PendingFrame{};
    }

    std::error_code ReconstructionPass::collectCompleted(const FenceCompletionValue completed) {
        std::ignore = m_deferred_releases.reclaim(completed);
        return {};
    }
} // namespace pP
