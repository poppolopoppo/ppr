module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.post_process_pass;
import :renderer.frame_fence;
import :renderer.presentation;
import :renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(PostProcess, debug, none)

    // ------------------------------------------------------------------
    // CPU/GPU constant-buffer ABI
    // ------------------------------------------------------------------

    namespace {
        // Mirrors post_process.slang: resolved extent, bloom combine controls
        // and the manual EV that is added to the adapted offset exactly once.
        struct alignas(16) PostParameters {
            u32 m_width;
            u32 m_height;
            float m_bloom_intensity;
            float m_manual_ev;
            u32 m_bloom_enabled;
            u32 m_reserved0;
            u32 m_reserved1;
            u32 m_reserved2;
        };
        static_assert(sizeof(PostParameters) == 32u and alignof(PostParameters) == 16u);
        static_assert(PPR_OFFSETOF(PostParameters, m_bloom_intensity) == 8u);
        static_assert(PPR_OFFSETOF(PostParameters, m_manual_ev) == 12u);
        static_assert(PPR_OFFSETOF(PostParameters, m_bloom_enabled) == 16u);

        // Mirrors present_scene.slang: exact integer resolved pixels, the single
        // SDR sRGB encoding decision for the acquired surface, and the
        // display-referred CAS strength (0 disables the cross taps).
        struct alignas(16) PresentParameters {
            u32 m_resolved_width;
            u32 m_resolved_height;
            u32 m_pixel_scale;
            u32 m_encode_srgb;
            float m_sharpen_strength;
            u32 m_reserved0;
            u32 m_reserved1;
            u32 m_reserved2;
        };
        static_assert(sizeof(PresentParameters) == 32u and alignof(PresentParameters) == 16u);
        static_assert(PPR_OFFSETOF(PresentParameters, m_pixel_scale) == 8u);
        static_assert(PPR_OFFSETOF(PresentParameters, m_encode_srgb) == 12u);
        static_assert(PPR_OFFSETOF(PresentParameters, m_sharpen_strength) == 16u);

        // Mirrors spd_bloom.slang. Threshold/soft knee are exposed units and
        // are divided by the total exposure once inside the prefilter.
        struct alignas(16) BloomParameters {
            u32 m_source_width;
            u32 m_source_height;
            u32 m_src_width;
            u32 m_src_height;
            u32 m_dst_width;
            u32 m_dst_height;
            float m_threshold;
            float m_soft_knee;
            float m_manual_ev;
            u32 m_level;
            u32 m_add_upsample;
            u32 m_reserved1;
            u32 m_reserved2;
            u32 m_reserved3;
            u32 m_reserved4;
            u32 m_reserved5;
        };
        static_assert(sizeof(BloomParameters) == 64u and alignof(BloomParameters) == 16u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_src_width) == 8u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_dst_width) == 16u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_threshold) == 24u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_manual_ev) == 32u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_level) == 36u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_add_upsample) == 40u);

        // Mirrors luminance_histogram.slang. Slang rounds this 24-byte field
        // extent up to a 16-byte multiple, so the CPU mirror carries the padding.
        struct alignas(16) HistogramParameters {
            u32 m_source_width;
            u32 m_source_height;
            u32 m_luminance_width;
            u32 m_luminance_height;
            u32 m_reserved0;
            u32 m_reserved1;
        };
        static_assert(sizeof(HistogramParameters) == 32u and alignof(HistogramParameters) == 16u);
        static_assert(PPR_OFFSETOF(HistogramParameters, m_luminance_width) == 8u);
        static_assert(PPR_OFFSETOF(HistogramParameters, m_reserved0) == 16u);

        struct alignas(16) AutoExposureParameters {
            float m_adaptation_alpha;
            float m_ev_min;
            float m_ev_max;
            u32 m_enabled;
        };
        static_assert(sizeof(AutoExposureParameters) == 16u and alignof(AutoExposureParameters) == 16u);
        static_assert(PPR_OFFSETOF(AutoExposureParameters, m_ev_min) == 4u);
        static_assert(PPR_OFFSETOF(AutoExposureParameters, m_ev_max) == 8u);
        static_assert(PPR_OFFSETOF(AutoExposureParameters, m_enabled) == 12u);

        constexpr std::array<const char *, 3> kBloomEntryPoints{"prefilterBloom", "downsampleBloom", "upsampleBloom"};
        constexpr std::array<const char *, 4> kHistogramEntryPoints{
            "clearHistogram", "computeLuminance", "buildHistogram", "reduceHistogram"};

        [[nodiscard]] rhi::BufferRange fullRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

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
            rhi::IDevice &device, slang::IModule *const module, const std::span<const char *const> entries,
            rhi::ComPtr<rhi::IShaderProgram> &out) {
            std::array<shader::ComPtr<slang::IEntryPoint>, 2> entry_points{};
            std::array<slang::IComponentType *, 2> components{};
            for (u32 index = 0u; index < entries.size(); ++index) {
                PPR_RETURN_ERROR_ON_FAIL(
                    PostProcess, module->findEntryPointByName(entries[index], entry_points[index].writeRef()));
                components[index] = entry_points[index].get();
            }

            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module;
            description.slangEntryPoints = components.data();
            description.slangEntryPointCount = static_cast<u32>(entries.size());
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, device.createShaderProgram(description, out.writeRef(), diagnostics.writeRef()));
            return {};
        }
    }

    // ------------------------------------------------------------------
    // histogram eye-adaptation reduction (CPU oracle)
    // ------------------------------------------------------------------

    float PostProcessPass::luminance(const float r, const float g, const float b) noexcept {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    float PostProcessPass::log2Luminance(const float lum) noexcept {
        // Clamp to the representable histogram floor before the logarithm so a
        // black frame maps to the lowest bin instead of -inf/NaN.
        return std::log2(std::max(lum, std::exp2(kLog2LuminanceMin)));
    }

    u32 PostProcessPass::luminanceBin(const float log2_lum) noexcept {
        const float span = kLog2LuminanceMax - kLog2LuminanceMin;
        const u32 bin = static_cast<u32>(
            std::clamp((log2_lum - kLog2LuminanceMin) / span, 0.0f, 1.0f) * float(kHistogramBins));
        return std::min(bin, kHistogramBins - 1u);
    }

    float PostProcessPass::histogramBinCenterLog2(const u32 bin) noexcept {
        const float span = kLog2LuminanceMax - kLog2LuminanceMin;
        return kLog2LuminanceMin + (float(bin) + 0.5f) * span / float(kHistogramBins);
    }

    float PostProcessPass::retainedMass(const std::span<const u32> histogram, const u32 total_texels) noexcept {
        if (total_texels == 0u) {
            return 0.0f;
        }

        // Fractional trim: the boundaries are count positions, not integer
        // bins, so a one-texel histogram retains its 0.8 interior exactly.
        // Decimal double percentiles keep that fractional mass exact before
        // the single float conversion.
        const double low = double(total_texels) * 0.10;
        const double high = double(total_texels) * 0.90;
        double retained = 0.0;
        double cumulative = 0.0;
        for (const u32 count: histogram) {
            const double start = cumulative;
            cumulative += double(count);
            retained += std::max(0.0, std::min(cumulative, high) - std::max(start, low));
        }

        return static_cast<float>(retained / double(total_texels));
    }

    float PostProcessPass::averageLog2Luminance(const std::span<const u32> histogram, const u32 total_texels) noexcept {
        if (total_texels == 0u) {
            return kLog2LuminanceMin;
        }

        const double low = double(total_texels) * 0.10;
        const double high = double(total_texels) * 0.90;
        double accumulated = 0.0;
        double accumulated_count = 0.0;
        double cumulative = 0.0;
        for (u32 bin = 0u; bin < histogram.size(); ++bin) {
            const double start = cumulative;
            cumulative += double(histogram[bin]);

            const double lo = std::max(start, low);
            const double hi = std::min(cumulative, high);
            if (lo < hi) {
                const double count = hi - lo;
                accumulated += double(histogramBinCenterLog2(bin)) * count;
                accumulated_count += count;
            }
        }

        return accumulated_count > 0.0 ? static_cast<float>(accumulated / accumulated_count) : kLog2LuminanceMin;
    }

    float PostProcessPass::targetEv(const float average_log2_lum, const float ev_min, const float ev_max) noexcept {
        // The target is applied as a radiance multiplier: scene * exp2(target).
        // Reverse the luminance ratio so middle gray maps to zero, darker scenes
        // get positive EV, and brighter scenes get negative EV.
        const float unclamped = std::log2(kMiddleGrayLuminance) - average_log2_lum;
        return std::clamp(unclamped, std::min(ev_min, ev_max), std::max(ev_min, ev_max));
    }

    float PostProcessPass::adaptationAlpha(const TimeSpan dt, const float tau) noexcept {
        if (dt <= TimeSpan::zero() or tau <= 0.0f) {
            return 0.0f;
        }

        return 1.0f - std::exp(-std::chrono::duration<float>(dt).count() / tau);
    }

    // ------------------------------------------------------------------
    // bloom level geometry (CPU oracle)
    // ------------------------------------------------------------------

    int2 PostProcessPass::bloomLevelExtent(const int2 resolved_extent, const u32 level) noexcept {
        int2 extent{std::max<i32>(1, (resolved_extent.x + 1) / 2), std::max<i32>(1, (resolved_extent.y + 1) / 2)};
        for (u32 step = 0u; step < std::min(level, kMaxBloomLevels); ++step) {
            extent = int2{std::max<i32>(1, (extent.x + 1) / 2), std::max<i32>(1, (extent.y + 1) / 2)};
        }

        return extent;
    }

    u32 PostProcessPass::bloomLevelCount(const int2 resolved_extent) noexcept {
        int2 extent{std::max<i32>(1, (resolved_extent.x + 1) / 2), std::max<i32>(1, (resolved_extent.y + 1) / 2)};
        u32 count = 1u;
        while (count < kMaxBloomLevels and (extent.x > 1 or extent.y > 1)) {
            extent = int2{std::max<i32>(1, (extent.x + 1) / 2), std::max<i32>(1, (extent.y + 1) / 2)};
            ++count;
        }

        return count;
    }

    std::array<int2, PostProcessPass::kMaxBloomLevels> PostProcessPass::bloomLevelExtents(
        const int2 resolved_extent) noexcept {
        std::array<int2, kMaxBloomLevels> extents{
            int2{zero_v}, int2{zero_v}, int2{zero_v}, int2{zero_v}, int2{zero_v}, int2{zero_v}};
        const u32 count = bloomLevelCount(resolved_extent);
        for (u32 level = 0u; level < kMaxBloomLevels; ++level) {
            extents[level] = bloomLevelExtent(resolved_extent, std::min(level, count - 1u));
        }

        return extents;
    }

    // ------------------------------------------------------------------
    // presentation contrast-adaptive sharpen (CPU oracle)
    // ------------------------------------------------------------------

    int2 PostProcessPass::resolvedPixelForWindowPixel(
        const int2 window_pixel, const int2 resolved_extent, const u32 pixel_scale) noexcept {
        // Truncating division matches the shader's integer divide; the crop clamp
        // then pins the right/bottom excess and any out-of-range input.
        const i32 scale = std::max<i32>(1, static_cast<i32>(pixel_scale));
        const int2 last{std::max<i32>(0, resolved_extent.x - 1), std::max<i32>(0, resolved_extent.y - 1)};
        return int2{std::clamp(window_pixel.x / scale, 0, last.x), std::clamp(window_pixel.y / scale, 0, last.y)};
    }

    float PostProcessPass::sharpenWeight(const float cross_min, const float cross_max) noexcept {
        // 1/sqrt(min + max + eps) grows as the local contrast falls. The floor
        // keeps black input finite and the ceiling bounds the cross-tap weight.
        const float contrast = cross_min + cross_max + kSharpenContrastEps;
        if (not (contrast > 0.0f)) {
            return kSharpenMaxWeight;
        }

        return std::min(1.0f / std::sqrt(contrast), kSharpenMaxWeight);
    }

    float PostProcessPass::sharpenChannel(
        const float center, const float left, const float right, const float up, const float down,
        const float strength) noexcept {
        // Zero strength (or a disabled control mapped to zero) is exactly the
        // unsharpened presentation: no cross tap is read at all.
        if (not (strength > 0.0f)) {
            return center;
        }

        const float peak = strength * (kSharpenPeakMin + (kSharpenPeakMax - kSharpenPeakMin) * strength);
        const float weight = sharpenWeight(
            std::min(std::min(left, right), std::min(up, down)), std::max(std::max(left, right), std::max(up, down)));
        const float cross = -peak * weight;

        // The centre keeps 1 - 4*cross so the five taps sum to one: a flat patch is
        // reproduced up to a rounding step, and only local contrast is amplified.
        const float sharpened = center * (1.0f - 4.0f * cross) + cross * (left + right + up + down);
        return std::clamp(sharpened, 0.0f, 1.0f);
    }

    // ------------------------------------------------------------------
    // initialization and teardown
    // ------------------------------------------------------------------

    std::error_code PostProcessPass::initialize(
        IShaderService &shader_service, const std::filesystem::path &content_dir) {
        const safe_ptr<IRhiService> rhi_service = IRhiService::get();
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, rhi_service != nullptr ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        rhi::IDevice &device = rhi_service->getDevice();

        bool initialization_complete = false;
        PPR_DEFER {
            if (not initialization_complete) {
                releaseGpuResources_();
            }
        };

        PPR_RETURN_ERROR_ON_FAIL(PostProcess, createPrograms_(shader_service, device, content_dir));

        // Presentation uses exact integer loads; bloom combines with bilinear
        // taps clamped to the valid coarse edge.
        rhi::SamplerDesc linear{};
        linear.minFilter = rhi::TextureFilteringMode::Linear;
        linear.magFilter = rhi::TextureFilteringMode::Linear;
        linear.mipFilter = rhi::TextureFilteringMode::Linear;
        linear.addressU = rhi::TextureAddressingMode::ClampToEdge;
        linear.addressV = rhi::TextureAddressingMode::ClampToEdge;
        linear.addressW = rhi::TextureAddressingMode::ClampToEdge;
        linear.maxAnisotropy = 1u;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createSampler(linear, m_linear_sampler.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, m_linear_sampler->getDescriptorHandle(&m_linear_sampler_handle));

        // The zero offset represents an unadapted first frame and the manual
        // exposure path. Both bind this finite 1x1 value instead of inventing one.
        const float zero = 0.0f;
        const rhi::SubresourceData initial{.data = &zero, .rowPitch = sizeof(zero), .slicePitch = sizeof(zero)};
        rhi::TextureDesc texture{};
        texture.type = rhi::TextureType::Texture2D;
        texture.size = {1u, 1u, 1u};
        texture.arrayLength = 1u;
        texture.mipCount = 1u;
        texture.format = rhi::Format::R32Float;
        texture.usage = rhi::TextureUsage::ShaderResource;
        texture.defaultState = rhi::ResourceState::ShaderResource;
        texture.label = "post zero exposure history";
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createTexture(texture, &initial, m_empty_exposure.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, m_empty_exposure->getDefaultView(m_empty_exposure_view.writeRef()));

        m_shader_service = safe_ptr{&shader_service};
        m_content_dir = content_dir;
        m_ready = true;
        initialization_complete = true;

        PPR_LOG(PostProcess, info, "PostProcessPass initialized");
        return {};
    }

    std::error_code PostProcessPass::createPrograms_(
        IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir) {
        // Post treatment: one RGBA16F resolved-size attachment, no depth.
        shader::SharedModule post_module{};
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess,
            shader_service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("post_process.slang"), "post_process", post_module.writeRef()));
        {
            auto *const layout = parameterLayout(post_module.get(), "g_post");
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                layout != nullptr and layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(PostParameters)
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                hasFieldOffset(layout, "m_bloom_intensity", PPR_OFFSETOF(PostParameters, m_bloom_intensity)) and
                        hasFieldOffset(layout, "m_manual_ev", PPR_OFFSETOF(PostParameters, m_manual_ev)) and
                        hasFieldOffset(layout, "m_bloom_enabled", PPR_OFFSETOF(PostParameters, m_bloom_enabled))
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, createProgram(
                             device, post_module.get(),
                             std::array<const char *, 2>{"vertexPostProcess", "fragmentPostProcess"}, m_post_program));

        rhi::ColorTargetDesc post_target{};
        post_target.format = rhi::Format::RGBA16Float;
        post_target.enableBlend = false;
        rhi::RenderPipelineDesc post_pipeline{};
        post_pipeline.program = m_post_program.get();
        post_pipeline.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        post_pipeline.targets = &post_target;
        post_pipeline.targetCount = 1u;
        post_pipeline.multisample.sampleCount = 1u;
        post_pipeline.depthStencil.format = rhi::Format::Undefined;
        post_pipeline.depthStencil.depthTestEnable = false;
        post_pipeline.depthStencil.depthWriteEnable = false;
        post_pipeline.rasterizer.cullMode = rhi::CullMode::None;
        post_pipeline.label = "post process pipeline";
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createRenderPipeline(post_pipeline, m_post_pipeline.writeRef()));

        // Presentation: exact integer resolved loads, display-referred CAS and
        // one sRGB decision.
        shader::SharedModule present_module{};
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, shader_service.loadModuleFromFile(
                             content_dir / TEXT("shaders") / TEXT("present_scene.slang"), "present_scene",
                             present_module.writeRef()));
        {
            auto *const layout = parameterLayout(present_module.get(), "g_present");
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                layout != nullptr and layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(PresentParameters)
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                hasFieldOffset(layout, "m_pixel_scale", PPR_OFFSETOF(PresentParameters, m_pixel_scale)) and
                        hasFieldOffset(layout, "m_encode_srgb", PPR_OFFSETOF(PresentParameters, m_encode_srgb)) and
                        hasFieldOffset(
                            layout, "m_sharpen_strength", PPR_OFFSETOF(PresentParameters, m_sharpen_strength))
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess,
            createProgram(
                device, present_module.get(), std::array<const char *, 2>{"vertexPresentScene", "fragmentPresentScene"},
                m_present_program));

        // Metering: clear/downsample/histogram/reduce entries.
        shader::SharedModule histogram_module{};
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, shader_service.loadModuleFromFile(
                             content_dir / TEXT("shaders") / TEXT("luminance_histogram.slang"), "luminance_histogram",
                             histogram_module.writeRef()));
        {
            auto *const layout = parameterLayout(histogram_module.get(), "g_luminance");
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                layout != nullptr and layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(HistogramParameters)
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                hasFieldOffset(layout, "luminanceExtent", PPR_OFFSETOF(HistogramParameters, m_luminance_width))
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
        }
        for (u32 index = 0u; index < m_histogram_programs.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, createProgram(
                                 device, histogram_module.get(), std::span{&kHistogramEntryPoints[index], 1u},
                                 m_histogram_programs[index]));
            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = m_histogram_programs[index];
            pipeline.label = kHistogramEntryPoints[index];
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, device.createComputePipeline(pipeline, m_histogram_pipelines[index].writeRef()));
        }

        // Adaptation: one 1x1 offset write.
        shader::SharedModule exposure_module{};
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, shader_service.loadModuleFromFile(
                             content_dir / TEXT("shaders") / TEXT("auto_exposure.slang"), "auto_exposure",
                             exposure_module.writeRef()));
        {
            auto *const layout = parameterLayout(exposure_module.get(), "g_auto_exposure");
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, layout != nullptr and
                                     layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(AutoExposureParameters)
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::invalid_argument));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess,
            createProgram(
                device, exposure_module.get(), std::array<const char *, 1>{"adaptExposure"}, m_auto_exposure_program));
        {
            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = m_auto_exposure_program;
            pipeline.label = "eye adaptation";
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, device.createComputePipeline(pipeline, m_auto_exposure_pipeline.writeRef()));
        }

        // Bloom: prefilter, per-level downsample, progressive upsample.
        shader::SharedModule bloom_module{};
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess,
            shader_service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("spd_bloom.slang"), "spd_bloom", bloom_module.writeRef()));
        {
            auto *const layout = parameterLayout(bloom_module.get(), "g_bloom");
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                layout != nullptr and layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(BloomParameters)
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                hasFieldOffset(layout, "m_threshold", PPR_OFFSETOF(BloomParameters, m_threshold)) and
                        hasFieldOffset(layout, "m_soft_knee", PPR_OFFSETOF(BloomParameters, m_soft_knee)) and
                        hasFieldOffset(layout, "m_manual_ev", PPR_OFFSETOF(BloomParameters, m_manual_ev)) and
                        hasFieldOffset(layout, "m_level", PPR_OFFSETOF(BloomParameters, m_level)) and
                        hasFieldOffset(layout, "m_add_upsample", PPR_OFFSETOF(BloomParameters, m_add_upsample))
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
        }
        for (u32 index = 0u; index < m_bloom_programs.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                createProgram(
                    device, bloom_module.get(), std::span{&kBloomEntryPoints[index], 1u}, m_bloom_programs[index]));
            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = m_bloom_programs[index];
            pipeline.label = kBloomEntryPoints[index];
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, device.createComputePipeline(pipeline, m_bloom_pipelines[index].writeRef()));
        }

        return {};
    }

    std::error_code PostProcessPass::createExposure_(rhi::IDevice &device, OutputSlot &slot) {
        const float zero = 0.0f;
        const rhi::SubresourceData initial{.data = &zero, .rowPitch = sizeof(zero), .slicePitch = sizeof(zero)};
        rhi::TextureDesc description{};
        description.type = rhi::TextureType::Texture2D;
        description.size = {1u, 1u, 1u};
        description.arrayLength = 1u;
        description.mipCount = 1u;
        description.format = rhi::Format::R32Float;
        description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::ShaderResource) | static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
        description.defaultState = rhi::ResourceState::ShaderResource;
        description.label = "post adapted exposure offset";
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createTexture(description, &initial, slot.m_exposure.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, slot.m_exposure->getDefaultView(slot.m_exposure_view.writeRef()));
        return {};
    }

    std::error_code PostProcessPass::createSlot_(
        rhi::IDevice &device, OutputSlot &slot, const int2 resolved_extent, const bool create_cohort) {
        if (not slot.m_exposure) {
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, createExposure_(device, slot));
        }

        if (not create_cohort) {
            return {};
        }

        const u32 width = safe_narrowing(resolved_extent.x);
        const u32 height = safe_narrowing(resolved_extent.y);

        rhi::TextureDesc output{};
        output.type = rhi::TextureType::Texture2D;
        output.size = {.width = width, .height = height, .depth = 1u};
        output.arrayLength = 1u;
        output.mipCount = 1u;
        output.format = rhi::Format::RGBA16Float;
        output.memoryType = rhi::MemoryType::DeviceLocal;
        output.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        output.defaultState = rhi::ResourceState::ShaderResource;
        output.label = "post display-linear output";

        rhi::TextureDesc luminance = output;
        luminance.size = {
            .width = std::min(width, kMaxLuminanceExtent),
            .height = std::min(height, kMaxLuminanceExtent),
            .depth = 1u};
        luminance.format = rhi::Format::R32Float;
        luminance.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::ShaderResource) | static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
        luminance.defaultState = rhi::ResourceState::UnorderedAccess;
        luminance.label = "post luminance downsample";

        const u32 bloom_levels = bloomLevelCount(resolved_extent);
        std::array<rhi::ComPtr<rhi::ITexture>, kMaxBloomLevels> bloom{};
        std::array<rhi::ComPtr<rhi::ITextureView>, kMaxBloomLevels> bloom_views{};
        for (u32 level = 0u; level < bloom_levels; ++level) {
            const int2 extent = bloomLevelExtent(resolved_extent, level);
            rhi::TextureDesc description = output;
            description.size = {.width = safe_narrowing(extent.x), .height = safe_narrowing(extent.y), .depth = 1u};
            description.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
            description.defaultState = rhi::ResourceState::UnorderedAccess;
            description.label = "post bloom level";
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createTexture(description, nullptr, bloom[level].writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, bloom[level]->getDefaultView(bloom_views[level].writeRef()));
        }

        rhi::ComPtr<rhi::ITexture> output_texture;
        rhi::ComPtr<rhi::ITextureView> output_view;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createTexture(output, nullptr, output_texture.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, output_texture->getDefaultView(output_view.writeRef()));

        rhi::ComPtr<rhi::ITexture> luminance_texture;
        rhi::ComPtr<rhi::ITextureView> luminance_view;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createTexture(luminance, nullptr, luminance_texture.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, luminance_texture->getDefaultView(luminance_view.writeRef()));

        const auto deviceBuffer = [&](const u64 bytes, const char *const label,
                                      rhi::ComPtr<rhi::IBuffer> &out) -> std::error_code {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = sizeof(u32);
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess));
            description.defaultState = rhi::ResourceState::UnorderedAccess;
            description.label = label;
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createBuffer(description, nullptr, out.writeRef()));
            return {};
        };
        rhi::ComPtr<rhi::IBuffer> histogram;
        rhi::ComPtr<rhi::IBuffer> target_ev;
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, deviceBuffer(kHistogramBins * sizeof(u32), "post luminance histogram", histogram));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, deviceBuffer(sizeof(float), "post histogram target EV", target_ev));

        rhi::ComPtr<rhi::IShaderObject> post_root;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, device.createRootShaderObject(m_post_program, post_root.writeRef()));
        rhi::ComPtr<rhi::IShaderObject> present_root;
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, device.createRootShaderObject(m_present_program, present_root.writeRef()));
        std::array<rhi::ComPtr<rhi::IShaderObject>, 4> histogram_roots{};
        for (u32 index = 0u; index < histogram_roots.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                device.createRootShaderObject(m_histogram_programs[index], histogram_roots[index].writeRef()));
        }
        std::array<rhi::ComPtr<rhi::IShaderObject>, 3> bloom_roots{};
        for (u32 index = 0u; index < bloom_roots.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, device.createRootShaderObject(m_bloom_programs[index], bloom_roots[index].writeRef()));
        }
        rhi::ComPtr<rhi::IShaderObject> auto_root;
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, device.createRootShaderObject(m_auto_exposure_program, auto_root.writeRef()));

        // Commit the new cohort together. The caller proved the slot's previous
        // use complete, so replacing these objects cannot invalidate in-flight work.
        slot.m_output = std::move(output_texture);
        slot.m_output_view = std::move(output_view);
        slot.m_luminance = std::move(luminance_texture);
        slot.m_luminance_view = std::move(luminance_view);
        slot.m_histogram = std::move(histogram);
        slot.m_target_ev = std::move(target_ev);
        slot.m_bloom = std::move(bloom);
        slot.m_bloom_views = std::move(bloom_views);
        slot.m_post_root = std::move(post_root);
        slot.m_present_root = std::move(present_root);
        slot.m_histogram_roots = std::move(histogram_roots);
        slot.m_bloom_roots = std::move(bloom_roots);
        slot.m_auto_exposure_root = std::move(auto_root);
        slot.m_extent = resolved_extent;
        slot.m_bloom_levels = bloom_levels;
        return {};
    }

    std::error_code PostProcessPass::shutdown() {
        PPR_LOG(PostProcess, info, "PostProcessPass shut down");
        releaseGpuResources_();
        return {};
    }

    std::error_code PostProcessPass::notifyDeviceLost() noexcept {
        // GPU objects are gone: drain deferred releases and drop every owned
        // resource without waiting.
        releaseGpuResources_();
        return {};
    }

    void PostProcessPass::releaseGpuResources_() noexcept {
        std::ignore = m_deferred_releases.drainAll();
        m_last_submitted_value = kFenceValueNone;
        m_slots = {};
        m_post_program.setNull();
        m_post_pipeline.setNull();
        m_present_program.setNull();
        m_histogram_programs = {};
        m_histogram_pipelines = {};
        m_auto_exposure_program.setNull();
        m_auto_exposure_pipeline.setNull();
        m_bloom_programs = {};
        m_bloom_pipelines = {};
        m_linear_sampler.setNull();
        m_linear_sampler_handle = {};
        m_empty_exposure_view.setNull();
        m_empty_exposure.setNull();
        m_present_pipeline.setNull();
        m_present_key.reset();
        m_shader_service = nullptr;
        m_content_dir.clear();

        m_cursor = 0u;
        m_selected = 0u;
        m_presented_slot = 0u;
        m_resolved_extent = int2{zero_v};
        m_frame_world = {};
        m_exposure_slot = 0u;
        m_exposure_valid = false;
        m_exposure_world = {};
        m_active_baseline = {};
        m_active_baseline_valid = false;
        m_pending_settings = {};
        m_pending_active_elapsed = {};
        m_pending_exposure_read = false;
        m_pending_exposure_read_slot = 0u;
        m_prepared = false;
        m_rendered = false;
        m_ready = false;
    }

    // ------------------------------------------------------------------
    // frame entry and resolved binding
    // ------------------------------------------------------------------

    std::error_code PostProcessPass::beginFrame(
        rhi::IDevice &device, const int2 resolved_extent, const GridWorldIdentity world,
        const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, resolved_extent.x > 0 and resolved_extent.y > 0
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        std::array<FenceCompletionValue, kFrameRingSlots> values{};
        for (u32 index = 0u; index < kFrameRingSlots; ++index) {
            values[index] = m_slots[index].m_last_use;
        }

        // The committed exposure texture is read as this frame's previous
        // offset; the same slot must never be selected for its write in the same
        // dispatch. Walk past it (and any incomplete slot) without a host wait.
        u32 selected = selectRingSlot(completed, values, m_cursor);
        for (u32 attempt = 0u; attempt < kFrameRingSlots; ++attempt) {
            if (not slotFenceComplete(completed, values[selected])) {
                return std::make_error_code(std::errc::device_or_resource_busy);
            }

            if (not m_exposure_valid or selected != m_exposure_slot) {
                break;
            }

            selected = (selected + 1u) % kFrameRingSlots;
        }

        if (m_exposure_valid and selected == m_exposure_slot) {
            // Capacity-busy is a transient outcome for the owner lane; no
            // host wait is introduced in steady-state rendering.
            return std::make_error_code(std::errc::device_or_resource_busy);
        }

        OutputSlot &slot = m_slots[selected];
        const bool recreate = not slot.m_output or
                              slot.m_extent.x != resolved_extent.x or
                              slot.m_extent.y != resolved_extent.y;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, createSlot_(device, slot, resolved_extent, recreate));

        m_cursor = selected;
        m_selected = selected;
        m_resolved_extent = resolved_extent;
        m_frame_world = world;
        m_prepared = false;
        m_rendered = false;
        m_pending_exposure_read = false;
        return {};
    }

    std::error_code PostProcessPass::prepare(
        const PreRenderContext &context, const ResolvedHdrView &view, const PostProcessSettings &settings,
        const TimeSpan active_elapsed) {
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, not m_rendered ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, validatePostProcessSettings(settings));

        // The binding must describe the selected resolved content: a positive
        // valid rectangle no larger than its backing, the selected extent, and
        // the frame's world identity. Backing may exceed R for padded resolve.
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, view.m_view and
                                 view.m_valid.x > 0 and
                                 view.m_valid.y > 0 and
                                 view.m_backing.x >= view.m_valid.x and
                                 view.m_backing.y >= view.m_valid.y
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, view.m_valid.x == m_resolved_extent.x and
                                 view.m_valid.y == m_resolved_extent.y and
                                 view.m_world == m_frame_world
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        rhi::ITexture *const texture = view.m_view->getTexture();
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, texture != nullptr ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        const rhi::TextureDesc &description = texture->getDesc();
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, description.type == rhi::TextureType::Texture2D and
                                 description.sampleCount == 1u and
                                 description.format == rhi::Format::RGBA16Float and
                                 description.size.width >= static_cast<u32>(view.m_valid.x) and
                                 description.size.height >= static_cast<u32>(view.m_valid.y)
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        OutputSlot &slot = m_slots[m_selected];
        // Retain the producer's binding through this submission's completion.
        slot.m_retained_view = view;
        slot.m_retained_submission = kFenceValueNone;

        context.m_encoder.setTextureState(texture, baseRange(), rhi::ResourceState::ShaderResource);

        m_pending_settings = settings;
        m_pending_active_elapsed = active_elapsed;
        {
            // Exposure metering/adaptation is encoded by its own dispatches; the
            // stable GPU marker attributes them without touching the algorithm.
            context.m_encoder.pushDebugGroup("Post.Exposure", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, encodeMetering_(context, slot));
        }
        if (settings.m_bloom_enabled) {
            context.m_encoder.pushDebugGroup("Post.Bloom", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, encodeBloom_(context, slot));
        } else {
            context.m_encoder.setTextureState(slot.m_bloom[0], baseRange(), rhi::ResourceState::ShaderResource);
        }

        m_prepared = true;
        return {};
    }

    rhi::ITexture &PostProcessPass::outputTexture() const {
        PPR_ASSERT(m_selected < kFrameRingSlots and m_slots[m_selected].m_output);
        return *m_slots[m_selected].m_output;
    }

    // ------------------------------------------------------------------
    // metering, adaptation and bloom encoding
    // ------------------------------------------------------------------

    std::error_code PostProcessPass::encodeMetering_(const PreRenderContext &context, OutputSlot &slot) {
        const hal::ProfileScope scope{"App.Pass.PostProcess.Metering"};
        const rhi::SubresourceRange range = baseRange();
        const u32 luminance_width = static_cast<u32>(std::min(slot.m_extent.x, static_cast<i32>(kMaxLuminanceExtent)));
        const u32 luminance_height = static_cast<u32>(std::min(slot.m_extent.y, static_cast<i32>(kMaxLuminanceExtent)));
        const HistogramParameters parameters{
            static_cast<u32>(slot.m_extent.x),
            static_cast<u32>(slot.m_extent.y),
            luminance_width,
            luminance_height,
            0u,
            0u,
        };

        const rhi::Binding source(slot.m_retained_view.m_view);
        const auto histogram_range = fullRange(slot.m_histogram);
        const auto target_range = fullRange(slot.m_target_ev);
        for (u32 index = 0u; index < slot.m_histogram_roots.size(); ++index) {
            rhi::ShaderCursor cursor{slot.m_histogram_roots[index]};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_luminance"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, constants.setData(&parameters, sizeof(parameters)));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_scene"].setBinding(source));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, cursor["g_luminance_image"].setBinding(rhi::Binding(slot.m_luminance_view)));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, cursor["g_histogram"].setBinding(rhi::Binding(slot.m_histogram, histogram_range)));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, cursor["g_target_ev"].setBinding(rhi::Binding(slot.m_target_ev, target_range)));
        }

        context.m_encoder.setTextureState(slot.m_luminance, range, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setBufferState(slot.m_histogram, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setBufferState(slot.m_target_ev, rhi::ResourceState::UnorderedAccess);

        const auto dispatch = [&](const u32 index, const u32 x, const u32 y) -> std::error_code {
            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->bindPipeline(m_histogram_pipelines[index], slot.m_histogram_roots[index]);
            pass->dispatchCompute(x, y, 1u);
            pass->end();
            return {};
        };

        if (m_pending_settings.m_auto_exposure) {
            context.m_encoder.pushDebugGroup("Post.Exposure.Histogram", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            const u32 groups_x = (luminance_width + 7u) / 8u;
            const u32 groups_y = (luminance_height + 7u) / 8u;
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, dispatch(0u, 1u, 1u)); // clearHistogram
            context.m_encoder.globalBarrier();
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, dispatch(1u, groups_x, groups_y)); // computeLuminance
            context.m_encoder.globalBarrier();
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, dispatch(2u, groups_x, groups_y)); // buildHistogram
            context.m_encoder.globalBarrier();
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, dispatch(3u, 1u, 1u)); // reduceHistogram
            context.m_encoder.globalBarrier();
        }

        // Adaptation is independent of reconstruction history: the previous
        // offset comes from the last accepted post frame in the same world.
        const bool history_valid = m_exposure_valid and m_frame_world == m_exposure_world;
        const rhi::Binding previous(history_valid ? m_slots[m_exposure_slot].m_exposure_view : m_empty_exposure_view);
        m_pending_exposure_read = history_valid;
        m_pending_exposure_read_slot = m_exposure_slot;

        const TimeSpan dt = m_active_baseline_valid ? std::max(TimeSpan{}, m_pending_active_elapsed - m_active_baseline)
                                                    : TimeSpan{};
        const AutoExposureParameters automatic{
            adaptationAlpha(dt, m_pending_settings.m_adaptation_tau),
            m_pending_settings.m_ev_min,
            m_pending_settings.m_ev_max,
            m_pending_settings.m_auto_exposure ? 1u : 0u,
        };

        rhi::ShaderCursor cursor{slot.m_auto_exposure_root};
        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_auto_exposure"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, constants.setData(&automatic, sizeof(automatic)));
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, cursor["g_target_ev"].setBinding(rhi::Binding(slot.m_target_ev, target_range)));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_previous_exposure"].setBinding(previous));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_exposure"].setBinding(rhi::Binding(slot.m_exposure_view)));

        context.m_encoder.setBufferState(slot.m_target_ev, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(slot.m_exposure, range, rhi::ResourceState::UnorderedAccess);
        {
            context.m_encoder.pushDebugGroup("Post.Exposure.Adaptation", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->bindPipeline(m_auto_exposure_pipeline, slot.m_auto_exposure_root);
            pass->dispatchCompute(1u, 1u, 1u);
            pass->end();
        }

        context.m_encoder.setTextureState(slot.m_exposure, range, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(slot.m_luminance, range, rhi::ResourceState::ShaderResource);
        return {};
    }

    std::error_code PostProcessPass::encodeBloom_(const PreRenderContext &context, OutputSlot &slot) {
        const hal::ProfileScope scope{"App.Pass.PostProcess.Bloom"};
        const rhi::SubresourceRange range = baseRange();
        BloomParameters parameters{};
        parameters.m_source_width = static_cast<u32>(slot.m_extent.x);
        parameters.m_source_height = static_cast<u32>(slot.m_extent.y);
        parameters.m_threshold = m_pending_settings.m_bloom_threshold;
        parameters.m_soft_knee = kBloomSoftKnee;
        parameters.m_manual_ev = m_pending_settings.m_exposure_ev;

        const auto dispatch = [&](const u32 index, const u32 x, const u32 y) -> std::error_code {
            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->bindPipeline(m_bloom_pipelines[index], slot.m_bloom_roots[index]);
            pass->dispatchCompute(x, y, 1u);
            pass->end();
            return {};
        };

        // Level 0: exposure-aware 2x2 prefilter into ceil(R/2).
        const int2 level0 = bloomLevelExtent(slot.m_extent, 0u);
        parameters.m_dst_width = static_cast<u32>(level0.x);
        parameters.m_dst_height = static_cast<u32>(level0.y);
        parameters.m_level = 0u;
        {
            rhi::ShaderCursor cursor{slot.m_bloom_roots[0]};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_bloom"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, constants.setData(&parameters, sizeof(parameters)));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, cursor["g_scene"].setBinding(rhi::Binding(slot.m_retained_view.m_view)));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_exposure"].setBinding(rhi::Binding(slot.m_exposure_view)));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_down_dst"].setBinding(rhi::Binding(slot.m_bloom_views[0])));
        }
        context.m_encoder.setTextureState(slot.m_bloom[0], range, rhi::ResourceState::UnorderedAccess);
        {
            context.m_encoder.pushDebugGroup("Post.Bloom.Prefilter", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                dispatch(0u, (static_cast<u32>(level0.x) + 7u) / 8u, (static_cast<u32>(level0.y) + 7u) / 8u));
        }
        context.m_encoder.globalBarrier();

        // Coarser levels: plain 2x2 boxes, edge-replicated, left unweighted.
        {
            context.m_encoder.pushDebugGroup("Post.Bloom.Downsample", {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };
            for (u32 level = 1u; level < slot.m_bloom_levels; ++level) {
                const int2 source = bloomLevelExtent(slot.m_extent, level - 1u);
                const int2 destination = bloomLevelExtent(slot.m_extent, level);
                parameters.m_src_width = static_cast<u32>(source.x);
                parameters.m_src_height = static_cast<u32>(source.y);
                parameters.m_dst_width = static_cast<u32>(destination.x);
                parameters.m_dst_height = static_cast<u32>(destination.y);
                parameters.m_level = level;

                rhi::ShaderCursor cursor{slot.m_bloom_roots[1]};
                rhi::ShaderCursor constants;
                PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_bloom"].getDereferenced(constants));
                PPR_RETURN_ERROR_ON_FAIL(PostProcess, constants.setData(&parameters, sizeof(parameters)));
                PPR_RETURN_ERROR_ON_FAIL(
                    PostProcess, cursor["g_down_src"].setBinding(rhi::Binding(slot.m_bloom_views[level - 1u])));
                PPR_RETURN_ERROR_ON_FAIL(
                    PostProcess, cursor["g_down_dst"].setBinding(rhi::Binding(slot.m_bloom_views[level])));

                context.m_encoder.setTextureState(slot.m_bloom[level - 1u], range, rhi::ResourceState::ShaderResource);
                context.m_encoder.setTextureState(slot.m_bloom[level], range, rhi::ResourceState::UnorderedAccess);
                PPR_RETURN_ERROR_ON_FAIL(
                    PostProcess,
                    dispatch(
                        1u, (static_cast<u32>(destination.x) + 7u) / 8u, (static_cast<u32>(destination.y) + 7u) / 8u));
                context.m_encoder.globalBarrier();
            }
        }

        // Weighted descent over unweighted raw levels:
        //   coarsest: result_last = w_last * raw_last
        //   otherwise: result_l = w_l * raw_l + tent(result_(l+1))
        // Each weight is applied once and the recursive reduction never
        // multiplies an already weighted level again.
        context.m_encoder.pushDebugGroup("Post.Bloom.Upsample", {});
        PPR_DEFER {
            context.m_encoder.popDebugGroup();
        };
        for (u32 level = slot.m_bloom_levels; level-- > 0u;) {
            // A single-level chain is already its own w0=1 result; no descent.
            if (slot.m_bloom_levels == 1u) {
                break;
            }

            const bool add_upsample = level + 1u < slot.m_bloom_levels;
            // The coarsest level has no coarser source; bind level 0 as an unused
            // SRV rather than aliasing the destination as both source and target.
            const u32 source_level = add_upsample ? level + 1u : 0u;
            const int2 source = bloomLevelExtent(slot.m_extent, source_level);
            const int2 destination = bloomLevelExtent(slot.m_extent, level);
            parameters.m_src_width = static_cast<u32>(source.x);
            parameters.m_src_height = static_cast<u32>(source.y);
            parameters.m_dst_width = static_cast<u32>(destination.x);
            parameters.m_dst_height = static_cast<u32>(destination.y);
            parameters.m_level = level;
            parameters.m_add_upsample = add_upsample ? 1u : 0u;

            rhi::ShaderCursor cursor{slot.m_bloom_roots[2]};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_bloom"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, constants.setData(&parameters, sizeof(parameters)));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, cursor["g_up_src"].setBinding(rhi::Binding(slot.m_bloom_views[source_level])));
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, cursor["g_up_dst"].setBinding(rhi::Binding(slot.m_bloom_views[level])));
            PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_linear"].setDescriptorHandle(m_linear_sampler_handle));

            context.m_encoder.setTextureState(slot.m_bloom[source_level], range, rhi::ResourceState::ShaderResource);
            context.m_encoder.setTextureState(slot.m_bloom[level], range, rhi::ResourceState::UnorderedAccess);
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess,
                dispatch(2u, (static_cast<u32>(destination.x) + 7u) / 8u, (static_cast<u32>(destination.y) + 7u) / 8u));
            context.m_encoder.globalBarrier();
        }

        context.m_encoder.setTextureState(slot.m_bloom[0], range, rhi::ResourceState::ShaderResource);
        return {};
    }

    // ------------------------------------------------------------------
    // display treatment and presentation
    // ------------------------------------------------------------------

    std::error_code PostProcessPass::render(const DrawContext &context) {
        const hal::ProfileScope scope{"App.Pass.PostProcess.Display"};
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, m_ready and m_prepared and not m_rendered
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        const RenderPipelineSignature &signature = context.m_render_pipeline_key;
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, signature.colorFormats().size() == 1u and
                                 signature.colorFormats().front() == rhi::Format::RGBA16Float and
                                 not signature.m_depth_stencil_format.has_value() and
                                 signature.m_sample_count == SampleCount::x1 and
                                 context.m_target_extent.x == m_resolved_extent.x and
                                 context.m_target_extent.y == m_resolved_extent.y
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        OutputSlot &slot = m_slots[m_selected];
        PostParameters parameters{};
        parameters.m_width = static_cast<u32>(slot.m_extent.x);
        parameters.m_height = static_cast<u32>(slot.m_extent.y);
        parameters.m_bloom_intensity = m_pending_settings.m_bloom_intensity;
        parameters.m_manual_ev = m_pending_settings.m_exposure_ev;
        parameters.m_bloom_enabled = m_pending_settings.m_bloom_enabled ? 1u : 0u;

        rhi::ShaderCursor cursor{slot.m_post_root};
        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_post"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, constants.setData(&parameters, sizeof(parameters)));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_scene"].setBinding(rhi::Binding(slot.m_retained_view.m_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess,
            cursor["g_bloom"].setBinding(
                rhi::Binding(
                    m_pending_settings.m_bloom_enabled ? slot.m_bloom_views[0] : slot.m_retained_view.m_view)));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_exposure"].setBinding(rhi::Binding(slot.m_exposure_view)));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_linear"].setDescriptorHandle(m_linear_sampler_handle));

        context.m_pass.pushDebugGroup("Post.ToneMap", {});
        PPR_DEFER {
            context.m_pass.popDebugGroup();
        };
        context.m_pass.setRenderState({
            .viewports = {context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {context.m_scissor},
            .scissorRectCount = 1u,
            .vertexBuffers = {},
            .indexBuffer = {},
        });
        context.m_pass.bindPipeline(m_post_pipeline, slot.m_post_root);
        context.m_pass.draw({.vertexCount = 3u});
        m_rendered = true;
        return {};
    }

    std::error_code PostProcessPass::ensurePresentPipeline_(
        rhi::IDevice &device, const RenderPipelineKey &surface_key) {
        if (m_present_key and static_cast<const RenderPipelineSignature &>(*m_present_key) !=
                                  static_cast<const RenderPipelineSignature &>(surface_key)) {
            if (m_present_pipeline) {
                m_deferred_releases.defer(
                    m_last_submitted_value, [held = std::move(m_present_pipeline)]() mutable { held.setNull(); });
            }
            m_present_key.reset();
        }

        if (not m_present_pipeline) {
            const RenderPipelineSignature &signature = surface_key;
            rhi::ColorTargetDesc target{};
            target.format = signature.colorFormats().front();
            target.enableBlend = false;
            rhi::RenderPipelineDesc description{};
            description.program = m_present_program.get();
            description.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            description.targets = &target;
            description.targetCount = 1u;
            description.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);
            description.depthStencil.format = rhi::Format::Undefined;
            description.depthStencil.depthTestEnable = false;
            description.depthStencil.depthWriteEnable = false;
            description.rasterizer.cullMode = rhi::CullMode::None;
            description.label = "post present pipeline";
            PPR_RETURN_ERROR_ON_FAIL(
                PostProcess, device.createRenderPipeline(description, m_present_pipeline.writeRef()));
            m_present_key.emplace(signature);
        }

        return {};
    }

    std::error_code PostProcessPass::present(const DrawContext &context, const PresentationGrid &grid) {
        const hal::ProfileScope scope{"App.Pass.PostProcess.Present"};
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, m_ready and m_selected < kFrameRingSlots and m_slots[m_selected].m_output
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        OutputSlot &slot = m_slots[m_selected];

        // The pass's own bookkeeping: the resolved extent this frame was begun
        // with and the user's pixel scale. A disagreement here means the wrong
        // slot, pipeline, or PresentParameters, so it stays fatal.
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, grid.m_resolved.x == slot.m_extent.x and
                                 grid.m_resolved.y == slot.m_extent.y and
                                 grid.m_pixel_scale >= 1u and
                                 grid.m_pixel_scale <= 8u
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        // The packet's captured window extent and the acquired image extent are
        // two independent observations. During an interactive drag the backend
        // materializes the live window size rather than the requested one, so
        // they legitimately disagree for a frame: that race is transient, and
        // the Renderer normally drops such a frame before present.
        if (grid.m_window.x != context.m_target_extent.x or grid.m_window.y != context.m_target_extent.y) [[unlikely]] {
            return std::make_error_code(std::errc::resource_unavailable_try_again);
        }

        const RenderPipelineSignature &signature = context.m_render_pipeline_key;
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, signature.colorFormats().size() == 1u and
                                 not signature.m_depth_stencil_format.has_value() and
                                 signature.m_sample_count == SampleCount::x1
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        // A hardware sRGB attachment encodes once; an SDR UNorm attachment asks
        // the final shader for the single encoding. Float targets stay linear.
        const rhi::Format format = signature.colorFormats().front();
        const bool encode_srgb = format == rhi::Format::RGBA8Unorm or format == rhi::Format::BGRA8Unorm;
        const bool linear_target = format == rhi::Format::RGBA16Float or format == rhi::Format::RGBA32Float;
        const bool srgb_target = format == rhi::Format::RGBA8UnormSrgb or format == rhi::Format::BGRA8UnormSrgb;
        PPR_RETURN_ERROR_ON_FAIL(
            PostProcess, linear_target or srgb_target or encode_srgb ? std::error_code{}
                                                                     : std::make_error_code(std::errc::not_supported));

        PPR_RETURN_ERROR_ON_FAIL(PostProcess, ensurePresentPipeline_(context.m_device, context.m_render_pipeline_key));

        PresentParameters parameters{};
        parameters.m_resolved_width = static_cast<u32>(slot.m_extent.x);
        parameters.m_resolved_height = static_cast<u32>(slot.m_extent.y);
        parameters.m_pixel_scale = grid.m_pixel_scale;
        parameters.m_encode_srgb = encode_srgb ? 1u : 0u;
        // A disabled control is the identity and skips every extra cross tap. The
        // shader sharpens resolved texels, so the same five loads serve every pixel
        // scale and the nearest enlargement replicates the result.
        parameters.m_sharpen_strength = m_pending_settings.m_sharpen_enabled ? m_pending_settings.m_sharpen_strength
                                                                             : 0.0f;

        rhi::ShaderCursor cursor{slot.m_present_root};
        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_present"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, constants.setData(&parameters, sizeof(parameters)));
        PPR_RETURN_ERROR_ON_FAIL(PostProcess, cursor["g_output"].setBinding(rhi::Binding(slot.m_output_view)));

        context.m_pass.setRenderState({
            .viewports = {context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {context.m_scissor},
            .scissorRectCount = 1u,
            .vertexBuffers = {},
            .indexBuffer = {},
        });
        context.m_pass.bindPipeline(m_present_pipeline, slot.m_present_root);
        context.m_pass.draw({.vertexCount = 3u});
        m_presented_slot = m_selected;
        return {};
    }

    bool PostProcessPass::retainedPictureAvailable(
        const int2 resolved_extent, const GridWorldIdentity world) const noexcept {
        if (not m_ready or not m_exposure_valid or m_exposure_slot >= kFrameRingSlots) {
            return false;
        }

        const OutputSlot &slot = m_slots[m_exposure_slot];
        return slot.m_output and
               m_exposure_world == world and
               slot.m_extent.x == resolved_extent.x and
               slot.m_extent.y == resolved_extent.y;
    }

    void PostProcessPass::selectRetainedPicture(const int2 resolved_extent) noexcept {
        if (not m_ready or not m_exposure_valid or m_exposure_slot >= kFrameRingSlots) {
            return;
        }

        const OutputSlot &slot = m_slots[m_exposure_slot];
        if (not slot.m_output or slot.m_extent.x != resolved_extent.x or slot.m_extent.y != resolved_extent.y) {
            return;
        }

        m_selected = m_exposure_slot;
        m_resolved_extent = resolved_extent;
        m_frame_world = m_exposure_world;
    }

    std::error_code PostProcessPass::finalize(const PreRenderContext &) {
        // The cleanup path owns no numeric status readback; Task 13's command
        // tail supplies the copy seam when a status cohort exists.
        return {};
    }

    // ------------------------------------------------------------------
    // frame settlement
    // ------------------------------------------------------------------

    void PostProcessPass::notifySubmitted(const FenceCompletionValue submit_value) noexcept {
        m_last_submitted_value = submit_value;
        if (m_selected < kFrameRingSlots) {
            OutputSlot &slot = m_slots[m_selected];
            slot.m_last_use = std::max(slot.m_last_use, submit_value);
            slot.m_retained_submission = submit_value;

            // The exposure history texture was read by this submission; extend
            // its reader fence so its slot cannot be rewritten before completion.
            if (m_pending_exposure_read and m_pending_exposure_read_slot < kFrameRingSlots) {
                OutputSlot &read = m_slots[m_pending_exposure_read_slot];
                read.m_last_use = std::max(read.m_last_use, submit_value);
            }

            // Accepted stage B commits the exposure role and the active-time
            // baseline. Rejected attempts never reach this function.
            m_exposure_slot = m_selected;
            m_exposure_valid = true;
            m_exposure_world = m_frame_world;
            m_active_baseline = m_pending_active_elapsed;
            m_active_baseline_valid = true;
        }

        m_pending_exposure_read = false;
        m_prepared = false;
        m_rendered = false;
    }

    void PostProcessPass::notifyPresented(const FenceCompletionValue present_value) noexcept {
        // Stage C (and any retained-picture draw) is a reader of the output
        // picture, so the slot stays pinned through that submission.
        if (m_presented_slot < kFrameRingSlots) {
            OutputSlot &slot = m_slots[m_presented_slot];
            slot.m_last_use = std::max(slot.m_last_use, present_value);
        }
    }

    void PostProcessPass::discardUnsubmitted() noexcept {
        if (m_selected < kFrameRingSlots) {
            OutputSlot &slot = m_slots[m_selected];
            slot.m_retained_view = {};
            slot.m_retained_submission = kFenceValueNone;
        }

        m_pending_exposure_read = false;
        m_prepared = false;
        m_rendered = false;
    }

    std::error_code PostProcessPass::collectCompleted(const FenceCompletionValue completed) {
        std::ignore = m_deferred_releases.reclaim(completed);
        for (OutputSlot &slot: m_slots) {
            if (not slot.m_retained_view.m_view) {
                continue;
            }

            if (slot.m_retained_submission == kFenceValueNone or
                slotFenceComplete(completed, slot.m_retained_submission)) {
                slot.m_retained_view = {};
                slot.m_retained_submission = kFenceValueNone;
            }
        }

        return {};
    }
}
