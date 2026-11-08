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
            u32 m_palette_enabled;
            float m_exposure_multiplier;
            u32 m_hdr_enabled;
            u32 m_encode_srgb;
            u32 m_bloom_enabled;
            float m_bloom_intensity;
        };
        static_assert(sizeof(RetroParameters) == 560u);
        static_assert(alignof(RetroParameters) == 16u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_source_extent) == 512u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_exposure_multiplier) == 528u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_encode_srgb) == 536u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_dither_strength) == 520u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_bloom_enabled) == 540u);
        static_assert(PPR_OFFSETOF(RetroParameters, m_bloom_intensity) == 544u);

        struct alignas(16) BloomParameters {
            u32 m_source_extent[2];
            u32 m_mip0_extent[2];
            float m_threshold;
            float m_soft_knee;
            u32 m_mip_count;
            u32 m_upsample_level;
        };
        static_assert(sizeof(BloomParameters) == 32u and alignof(BloomParameters) == 16u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_mip0_extent) == 8u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_threshold) == 16u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_soft_knee) == 20u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_mip_count) == 24u);
        static_assert(PPR_OFFSETOF(BloomParameters, m_upsample_level) == 28u);

        // CPU mirrors of the histogram/adaptation Slang constant buffers.
        struct LuminanceHistogramParameters {
            u32 m_source_extent[2];
            u32 m_luminance_extent[2];
            u32 m_reserved0;
            u32 m_reserved1;
        };
        static_assert(sizeof(LuminanceHistogramParameters) == 24u and alignof(LuminanceHistogramParameters) == 4u);
        static_assert(PPR_OFFSETOF(LuminanceHistogramParameters, m_luminance_extent) == 8u);
        static_assert(PPR_OFFSETOF(LuminanceHistogramParameters, m_reserved0) == 16u);
        static_assert(PPR_OFFSETOF(LuminanceHistogramParameters, m_reserved1) == 20u);

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

    RenderPipelineSignature RetroPass::scenePipelineSignature(const bool temporal) noexcept {
        RenderPipelineSignature signature{};
        signature.m_color_formats[0] = rhi::Format::RGBA16Float;
        signature.m_color_format_count = 1u;
        signature.m_depth_stencil_format = rhi::Format::D32Float;
        signature.m_sample_count = temporal ? SampleCount::x4 : SampleCount::x1;
        return signature;
    }

    RenderPipelineSignature RetroPass::temporal2xScenePipelineSignature() noexcept {
        auto signature = scenePipelineSignature(true);
        signature.m_color_formats[1] = rhi::Format::R8Unorm;
        signature.m_color_format_count = 2u;
        return signature;
    }

    float2 RetroPass::temporalJitter(const u32 phase) noexcept {
        // Filmic Worlds' half-pixel 2x2 cycle, centered about zero.
        return float2{(phase & 1u) != 0u ? 0.25f : -0.25f, (phase & 2u) != 0u ? 0.25f : -0.25f};
    }

    // ------------------------------------------------------------------
    // histogram eye-adaptation reduction (CPU oracle)
    // ------------------------------------------------------------------

    float RetroPass::luminance(const float r, const float g, const float b) noexcept {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    float RetroPass::log2Luminance(const float lum) noexcept {
        // Clamp to the representable histogram floor before the logarithm so a
        // black frame maps to the lowest bin instead of -inf/NaN.
        return std::log2(std::max(lum, std::exp2(kLog2LuminanceMin)));
    }

    u32 RetroPass::luminanceBin(const float log2_lum) noexcept {
        const float span = kLog2LuminanceMax - kLog2LuminanceMin;
        const u32 bin = static_cast<u32>(
            std::clamp((log2_lum - kLog2LuminanceMin) / span, 0.0f, 1.0f) * float(kHistogramBins));
        return std::min(bin, kHistogramBins - 1u);
    }

    float RetroPass::histogramBinCenterLog2(const u32 bin) noexcept {
        const float span = kLog2LuminanceMax - kLog2LuminanceMin;
        return kLog2LuminanceMin + (float(bin) + 0.5f) * span / float(kHistogramBins);
    }

    float RetroPass::averageLog2Luminance(const std::span<const u32> histogram, const u32 total_pixels) noexcept {
        if (total_pixels == 0u) {
            return kLog2LuminanceMin;
        }

        const float low = float(total_pixels) * kHistogramLowPercentile;
        const float high = float(total_pixels) * kHistogramHighPercentile;

        float accumulated = 0.0f;
        u32 accumulated_count = 0u;
        u32 cumulative = 0u;
        for (u32 bin = 0u; bin < histogram.size(); ++bin) {
            const u32 bin_start = cumulative;
            cumulative += histogram[bin];

            const float lo = std::max(float(bin_start), low);
            const float hi = std::min(float(cumulative), high);
            if (lo < hi) {
                const u32 count = static_cast<u32>(hi - lo);
                accumulated += histogramBinCenterLog2(bin) * float(count);
                accumulated_count += count;
            }
        }

        return accumulated_count != 0u ? accumulated / float(accumulated_count) : kLog2LuminanceMin;
    }

    float RetroPass::targetEv(const float average_log2_lum, const float ev_min, const float ev_max) noexcept {
        // The GPU reduction writes the unclamped target; the final clamp happens
        // in adaptExposure. This mirrors that final clamp for the CPU oracle.
        const float unclamped = average_log2_lum - std::log2(kMiddleGrayLuminance);
        return std::clamp(unclamped, std::min(ev_min, ev_max), std::max(ev_min, ev_max));
    }

    float RetroPass::adaptationAlpha(const TimeSpan dt, const float tau) noexcept {
        if (dt <= TimeSpan::zero() or tau <= 0.0f) {
            return 0.0f;
        }

        return 1.0f - std::exp(-std::chrono::duration<float>(dt).count() / tau);
    }

    // ------------------------------------------------------------------
    // bloom chain geometry (CPU oracle)
    // ------------------------------------------------------------------

    int2 RetroPass::bloomMip0Extent(const int2 output_extent) noexcept {
        const auto axis = [](const i32 extent) { return std::max<i32>(1, (extent + 1) / 2); };
        return int2{axis(output_extent.x), axis(output_extent.y)};
    }

    int2 RetroPass::bloomMipExtent(const int2 mip0_extent, const u32 level) noexcept {
        const auto axis = [level](const i32 extent) { return std::max<i32>(1, extent >> std::min(level, 31u)); };
        return int2{axis(mip0_extent.x), axis(mip0_extent.y)};
    }

    u32 RetroPass::bloomMipCount(const int2 mip0_extent) noexcept {
        const u32 largest = static_cast<u32>(std::max(mip0_extent.x, mip0_extent.y));
        const u32 full = largest > 0u ? static_cast<u32>(std::bit_width(largest)) : 1u;
        return std::min(full, kMaxBloomMips);
    }

    u64 RetroPass::bloomChainBytes(const int2 mip0_extent) noexcept {
        const u32 count = bloomMipCount(mip0_extent);
        u64 bytes = 0u;
        for (u32 level = 0u; level < count; ++level) {
            const int2 extent = bloomMipExtent(mip0_extent, level);
            bytes += static_cast<u64>(extent.x) * static_cast<u64>(extent.y) * 8u;
        }
        return bytes;
    }

    Expected<CameraSnapshot> RetroPass::beginFrame(
        const RetroSettings &settings, const CameraSnapshot &camera, const int2 extent, const u64 world_generation,
        const u64 world_job, const u64 world_content, const TimeSpan request_elapsed, const TimeSpan frame_delta,
        const u64 lighting_revision, const RetroLightingPublication lighting_publication) {
        return beginFrame(
            settings, camera, extent, extent, world_generation, world_job, world_content, request_elapsed, frame_delta,
            lighting_revision, lighting_publication);
    }

    std::error_code RetroPass::validateFrameSettings(
        const RetroSettings &settings, const int2 raster_extent, const int2 output_extent) noexcept {
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, std::isfinite(settings.m_exposure_ev) and
                               settings.m_exposure_ev >= -16.0f and
                               settings.m_exposure_ev <= 16.0f
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, not settings.m_bloom_enabled or (settings.m_hdr_enabled and (settings.m_spatial_enabled or
                                                                                       settings.m_temporal_2x_enabled))
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        if (settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, not settings.m_temporal_enabled and not settings.m_spatial_enabled
                               ? std::error_code{}
                               : std::make_error_code(std::errc::invalid_argument));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, not settings.m_auto_exposure or (settings.m_hdr_enabled and
                                                           settings.m_temporal_2x_enabled and
                                                           std::isfinite(settings.m_adaptation_tau) and
                                                           settings.m_adaptation_tau > 0.0f and
                                                           std::isfinite(settings.m_ev_min) and
                                                           std::isfinite(settings.m_ev_max) and
                                                           settings.m_ev_min <= settings.m_ev_max)
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        if (settings.m_spatial_enabled or settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, not settings.m_enabled and
                                   not settings.m_temporal_enabled and
                                   raster_extent.x > 0 and
                                   raster_extent.y > 0 and
                                   output_extent.x > 0 and
                                   output_extent.y > 0 and
                                   output_extent.x % 2 == 0 and
                                   output_extent.y % 2 == 0 and
                                   raster_extent.x == output_extent.x / 2 and
                                   raster_extent.y == output_extent.y / 2
                               ? std::error_code{}
                               : std::make_error_code(std::errc::invalid_argument));
        }

        return {};
    }

    Expected<CameraSnapshot> RetroPass::beginFrame(
        const RetroSettings &settings, const CameraSnapshot &camera, const int2 extent, const int2 output_extent,
        const u64 world_generation, const u64 world_job, const u64 world_content, const TimeSpan request_elapsed,
        const TimeSpan frame_delta, const u64 lighting_revision, const RetroLightingPublication lighting_publication) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(RetroPass, validateFrameSettings(settings, extent, output_extent));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RetroPass, not settings.m_spatial_enabled or m_spatial_supported
                           ? std::error_code{}
                           : std::make_error_code(std::errc::not_supported));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RetroPass, not settings.m_bloom_enabled or m_bloom_supported
                           ? std::error_code{}
                           : std::make_error_code(std::errc::not_supported));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RetroPass, not settings.m_temporal_2x_enabled or m_temporal_2x_supported
                           ? std::error_code{}
                           : std::make_error_code(std::errc::not_supported));

        RetroSettings effective = settings;
        effective.m_temporal_enabled = settings.m_temporal_enabled and
                                       m_temporal_supported and
                                       extent.x > 0 and
                                       extent.y > 0;
        if (settings.m_temporal_enabled and not m_temporal_supported and not m_warned_unsupported) {
            // Unsupported devices keep the requested look through the spatial
            // path. The caller's independent requested settings stay intact.
            PPR_LOG(RetroPass, warning, "Temporal supersampling unavailable: multisample shader loads unsupported");
            m_warned_unsupported = true;
        }

        const std::array<u64, 3u> identity{world_generation, world_job, world_content};
        if (effective.m_temporal_2x_enabled) {
            // Retained presentation restores display state, not the identity or
            // camera of the latest accepted history producer.
            const auto &previous = m_scene_slots[m_history_slot];
            m_temporal_2x_history_eligible = m_history_valid and
                                             previous.m_temporal_2x and
                                             not camera.m_has_camera_cut and
                                             identity == previous.m_picture_identity and
                                             lighting_revision == previous.m_picture_lighting_revision and
                                             lighting_publication == previous.m_picture_lighting_publication and
                                             camera.m_camera_mode == previous.m_picture_camera.m_camera_mode and
                                             extent.x == previous.m_extent.x and
                                             extent.y == previous.m_extent.y and
                                             output_extent.x == previous.m_output_extent.x and
                                             output_extent.y == previous.m_output_extent.y;
            m_frame_phase = m_temporal_2x_history_eligible ? m_phase : 0u;
        } else {
            // A proposed mode switch cannot invalidate or release accepted TSR
            // history. The protected slot survives until a fresh submit commits.
            const bool protects_temporal2x = m_history_valid and m_scene_slots[m_history_slot].m_temporal_2x;
            if (not protects_temporal2x and (camera.m_has_camera_cut or
                                                identity != m_content_identity or
                                                lighting_revision != m_content_lighting_revision or
                                                camera.m_camera_mode != m_camera.m_camera_mode or
                                                extent.x != m_frame_extent.x or
                                                extent.y != m_frame_extent.y or
                                                output_extent.x != m_output_extent.x or
                                                output_extent.y != m_output_extent.y or
                                                effective.m_enabled != m_settings.m_enabled or
                                                effective.m_pixel_scale != m_settings.m_pixel_scale or
                                                effective.m_palette_enabled != m_settings.m_palette_enabled or
                                                effective.m_temporal_enabled != m_settings.m_temporal_enabled or
                                                effective.m_spatial_enabled != m_settings.m_spatial_enabled or
                                                m_settings.m_temporal_2x_enabled)) {
                m_history_valid = false;
                m_phase = 0u;
            }

            m_frame_phase = protects_temporal2x ? 0u : m_phase;
        }

        m_settings = effective;
        m_frame_extent = extent;
        m_output_extent = output_extent;
        m_content_identity = identity;
        m_content_lighting_revision = lighting_revision;
        m_lighting_publication = lighting_publication;
        m_request_elapsed = request_elapsed;
        m_frame_delta = frame_delta;
        m_camera = camera;
        m_scene_recorded = false;
        m_history_used = false;

        CameraSnapshot raster = camera;
        if (effective.m_temporal_enabled or effective.m_temporal_2x_enabled) {
            const float2 jitter = temporalJitter(m_frame_phase) * 2.0f / float2{float(extent.x), float(extent.y)};
            raster.m_projection = camera.m_projection * makeJitterMatrix(jitter);
            raster.m_view_projection = camera.m_view_projection * makeJitterMatrix(jitter);
            raster.m_invert_projection = inverse(raster.m_projection);
            raster.m_invert_view_projection = inverse(raster.m_view_projection);
            raster.m_jitter = jitter;
            raster.m_jittered_projection = raster.m_projection;
            raster.m_jittered_view_projection = raster.m_view_projection;
            raster.m_invert_jittered_view_projection = raster.m_invert_view_projection;
        }

        return raster;
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

        // Capability fallback is decided before raster jitter is applied.
        m_temporal_supported = true;
        for (const auto format: {rhi::Format::RGBA16Float, rhi::Format::D32Float}) {
            rhi::FormatSupport support{};
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.getFormatSupport(format, &support));
            const u32 required = static_cast<u32>(rhi::FormatSupport::Multisampling) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderLoad);
            m_temporal_supported = m_temporal_supported and (static_cast<u32>(support) & required) == required;
            if (format == rhi::Format::RGBA16Float) {
                const u32 bloom_required = static_cast<u32>(rhi::FormatSupport::ShaderLoad) |
                                           static_cast<u32>(rhi::FormatSupport::ShaderUavStore);
                m_bloom_supported = (static_cast<u32>(support) & bloom_required) == bloom_required;
            }
        }
        if (m_temporal_supported) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, createTemporalProgram_(shader_service, device, content_dir));
        }

        // Spatial has no fallback. Program creation stays lazy so an unused
        // opt-in cannot fail ordinary initialization on another backend.
        m_spatial_supported = device.hasFeature("standard-sample-locations-4") and
                              device.hasFeature("sample-rate-shading");
        rhi::FormatSupport reactive_support{};
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.getFormatSupport(rhi::Format::R8Unorm, &reactive_support));
        const u32 reactive_required = static_cast<u32>(rhi::FormatSupport::Multisampling) |
                                      static_cast<u32>(rhi::FormatSupport::RenderTarget) |
                                      static_cast<u32>(rhi::FormatSupport::Blendable) |
                                      static_cast<u32>(rhi::FormatSupport::ShaderLoad);
        m_temporal_2x_supported = m_spatial_supported and
                                  m_temporal_supported and
                                  m_bloom_supported and
                                  device.hasFeature("independent-blend-enabled") and
                                  (static_cast<u32>(reactive_support) & reactive_required) == reactive_required;
        m_shader_service = safe_ptr{&shader_service};
        m_content_dir = content_dir;

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

        auto *const layout = retro_shader->getLayout();
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, layout != nullptr ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        slang::TypeLayoutReflection *constants = nullptr;
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_retro") {
                constants = parameter->getTypeLayout()->getElementTypeLayout();
                break;
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, constants != nullptr and
                               constants->getSize() == sizeof(RetroParameters) and
                               constants->getFieldCount() == 9u
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            constants->getFieldByIndex(1u)->getOffset() == PPR_OFFSETOF(RetroParameters, m_source_extent) and
                    constants->getFieldByIndex(4u)->getOffset() ==
                        PPR_OFFSETOF(RetroParameters, m_exposure_multiplier) and
                    constants->getFieldByIndex(6u)->getOffset() == PPR_OFFSETOF(RetroParameters, m_encode_srgb) and
                    constants->getFieldByIndex(7u)->getOffset() == PPR_OFFSETOF(RetroParameters, m_bloom_enabled) and
                    constants->getFieldByIndex(8u)->getOffset() == PPR_OFFSETOF(RetroParameters, m_bloom_intensity)
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));

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

        // The present shader always samples g_exposure; the zero offset texture
        // keeps the fixed-exposure path (auto-exposure off) a no-op and serves as
        // the first frame's previous-exposure history for adaptation.
        const float empty_exposure = 0.0f;
        const rhi::SubresourceData initial{
            .data = &empty_exposure, .rowPitch = sizeof(empty_exposure), .slicePitch = sizeof(empty_exposure)};
        rhi::TextureDesc texture{};
        texture.type = rhi::TextureType::Texture2D;
        texture.size = {1u, 1u, 1u};
        texture.arrayLength = 1u;
        texture.mipCount = 1u;
        texture.format = rhi::Format::R32Float;
        texture.usage = rhi::TextureUsage::ShaderResource;
        texture.defaultState = rhi::ResourceState::ShaderResource;
        texture.label = "auto-exposure empty history";
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(texture, &initial, m_empty_exposure.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, m_empty_exposure->getDefaultView(m_empty_exposure_view.writeRef()));

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

        m_temporal_2x_roots = {};
        m_temporal_2x_pipeline.setNull();
        m_temporal_2x_program.setNull();
        m_temporal_2x_supported = false;
        m_temporal_2x_history_eligible = false;
        m_frame_phase = 0u;
        m_settings.m_temporal_2x_enabled = false;
        m_lighting_publication = {};
        m_last_temporal_2x_submission.reset();

        m_bloom_roots = {};
        m_bloom_pipelines = {};
        m_bloom_programs = {};
        m_bloom_supported = false;
        m_settings.m_bloom_enabled = false;

        m_histogram_roots = {};
        m_histogram_pipelines = {};
        m_histogram_programs = {};
        m_auto_exposure_roots = {};
        m_auto_exposure_pipeline.setNull();
        m_auto_exposure_program.setNull();
        m_settings.m_auto_exposure = false;

        m_spatial_roots = {};
        m_spatial_pipeline.setNull();
        m_spatial_program.setNull();
        m_shader_service = nullptr;
        m_content_dir.clear();
        m_spatial_supported = false;
        m_settings.m_spatial_enabled = false;

        m_temporal_roots = {};
        m_temporal_pipeline.setNull();
        m_temporal_program.setNull();
        m_empty_history_view.setNull();
        m_empty_history.setNull();
        m_warned_unsupported = false;
        m_history_valid = false;
        m_history_used = false;
        m_temporal_supported = false;
        m_phase = 0u;
        m_pipeline.setNull();
        m_pipeline_key.reset();
        m_scene_roots = {};
        m_sampler.setNull();
        m_sampler_handle = {};
        m_program.setNull();
        m_empty_exposure_view.setNull();
        m_empty_exposure.setNull();

        m_scene_slots = {};
        m_scene_cursor = 0u;
        m_scene_slot = 0u;
        m_retain_completed_picture = false;
        m_completed_picture_valid = false;
        m_scene_sampled = false;
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
        const bool multisampled = m_settings.m_temporal_enabled or
                                  m_settings.m_spatial_enabled or
                                  m_settings.m_temporal_2x_enabled;
        color_desc.type = multisampled ? rhi::TextureType::Texture2DMS : rhi::TextureType::Texture2D;
        color_desc.sampleCount = multisampled ? 4u : 1u;
        color_desc.size = {.width = width, .height = height, .depth = 1u};
        color_desc.arrayLength = 1u;
        color_desc.mipCount = 1u;
        color_desc.format = rhi::Format::RGBA16Float;
        color_desc.memoryType = rhi::MemoryType::DeviceLocal;
        color_desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        color_desc.defaultState = rhi::ResourceState::ShaderResource;
        color_desc.label = "retro scene color";

        rhi::TextureDesc depth_desc{};
        depth_desc.type = color_desc.type;
        depth_desc.sampleCount = color_desc.sampleCount;
        depth_desc.size = {.width = width, .height = height, .depth = 1u};
        depth_desc.arrayLength = 1u;
        depth_desc.mipCount = 1u;
        depth_desc.format = rhi::Format::D32Float;
        depth_desc.memoryType = rhi::MemoryType::DeviceLocal;
        depth_desc.usage = multisampled ? static_cast<rhi::TextureUsage>(
                                              static_cast<u32>(rhi::TextureUsage::DepthStencil) |
                                              static_cast<u32>(rhi::TextureUsage::ShaderResource))
                                        : rhi::TextureUsage::DepthStencil;
        depth_desc.defaultState = rhi::ResourceState::DepthWrite;
        depth_desc.label = "retro scene depth";

        auto spatial_desc = color_desc;
        spatial_desc.type = rhi::TextureType::Texture2D;
        spatial_desc.sampleCount = 1u;
        spatial_desc.size = {.width = width, .height = height, .depth = 1u};
        spatial_desc.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::ShaderResource) | static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
        spatial_desc.label = "spatial linear HDR output";
        if (m_settings.m_spatial_enabled or m_settings.m_temporal_2x_enabled) {
            spatial_desc.size.width = safe_narrowing(m_output_extent.x);
            spatial_desc.size.height = safe_narrowing(m_output_extent.y);

            // The pinned Vulkan guard checks these exact committed descriptor
            // roles before creating any image. No initial data or placed heap.
            for (const auto &descriptor: {color_desc, depth_desc, spatial_desc}) {
                std::size_t size{}, alignment{};
                PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.getTextureAllocationInfo(descriptor, &size, &alignment));
                PPR_RETURN_ERROR_ON_FAIL(
                    RetroPass, size > 0u and alignment > 0u ? std::error_code{}
                                                            : std::make_error_code(std::errc::invalid_argument));
            }
        }

        const int2 bloom_mip0 = bloomMip0Extent(m_output_extent);
        const u32 bloom_mip_count = bloomMipCount(bloom_mip0);
        auto bloom_desc = spatial_desc;
        bloom_desc.size = {.width = safe_narrowing(bloom_mip0.x), .height = safe_narrowing(bloom_mip0.y), .depth = 1u};
        // The chain depth tracks the logical full chain (down to 1x1, capped at
        // six); Vulkan rejects a mipCount beyond floor(log2(extent)) + 1, so a
        // tiny extent must not force the full six mips.
        bloom_desc.mipCount = bloom_mip_count;
        bloom_desc.label = "scene-linear SPD bloom chain";
        if (m_settings.m_bloom_enabled) {
            std::size_t size{}, alignment{};
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.getTextureAllocationInfo(bloom_desc, &size, &alignment));
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass,
                size > 0u and alignment > 0u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        auto reactive_desc = color_desc;
        reactive_desc.format = rhi::Format::R8Unorm;
        reactive_desc.label = "temporal2x reactive sample coverage";
        if (m_settings.m_temporal_2x_enabled) {
            for (const auto &descriptor: {reactive_desc, spatial_desc}) {
                std::size_t size{}, alignment{};
                PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.getTextureAllocationInfo(descriptor, &size, &alignment));
                PPR_RETURN_ERROR_ON_FAIL(
                    RetroPass, size > 0u and alignment > 0u ? std::error_code{}
                                                            : std::make_error_code(std::errc::invalid_argument));
            }
        }

        rhi::ComPtr<rhi::ITexture> reactive;
        rhi::ComPtr<rhi::ITextureView> reactive_view;
        rhi::ComPtr<rhi::IShaderObject> temporal_2x_root;
        if (m_settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(reactive_desc, nullptr, reactive.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, reactive->getDefaultView(reactive_view.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device.createRootShaderObject(m_temporal_2x_program, temporal_2x_root.writeRef()));
        }

        rhi::ComPtr<rhi::ITexture> color;
        rhi::ComPtr<rhi::ITextureView> color_view;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(color_desc, nullptr, color.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, color->getDefaultView(color_view.writeRef()));

        rhi::ComPtr<rhi::ITexture> depth;
        rhi::ComPtr<rhi::ITextureView> depth_view;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(depth_desc, nullptr, depth.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, depth->getDefaultView(depth_view.writeRef()));

        rhi::ComPtr<rhi::ITexture> history;
        rhi::ComPtr<rhi::ITextureView> history_view;
        if (m_settings.m_temporal_enabled) {
            color_desc.type = rhi::TextureType::Texture2D;
            color_desc.sampleCount = 1u;
            color_desc.label = "temporal linear history";
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(color_desc, nullptr, history.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, history->getDefaultView(history_view.writeRef()));
        }

        if (m_settings.m_temporal_2x_enabled) {
            auto history_desc = spatial_desc;
            history_desc.label = "temporal2x scene-linear output history";
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(history_desc, nullptr, history.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, history->getDefaultView(history_view.writeRef()));
        }

        rhi::ComPtr<rhi::ITexture> spatial;
        rhi::ComPtr<rhi::ITextureView> spatial_view;
        if (m_settings.m_spatial_enabled or m_settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(spatial_desc, nullptr, spatial.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, spatial->getDefaultView(spatial_view.writeRef()));
        }

        rhi::ComPtr<rhi::ITexture> bloom;
        rhi::ComPtr<rhi::ITextureView> bloom_view;
        std::array<rhi::ComPtr<rhi::ITextureView>, kMaxBloomMips> bloom_uav_views{};
        std::array<rhi::ComPtr<rhi::IShaderObject>, 3> bloom_roots{};
        if (m_settings.m_bloom_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(bloom_desc, nullptr, bloom.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, bloom->getDefaultView(bloom_view.writeRef()));
            for (u32 level = 0u; level < bloom_mip_count; ++level) {
                rhi::TextureViewDesc view_desc{};
                view_desc.subresourceRange = {.layer = 0u, .layerCount = 1u, .mip = level, .mipCount = 1u};
                PPR_RETURN_ERROR_ON_FAIL(RetroPass, bloom->createView(view_desc, bloom_uav_views[level].writeRef()));
            }
            for (u32 index = 0u; index < bloom_roots.size(); ++index) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RetroPass, device.createRootShaderObject(m_bloom_programs[index], bloom_roots[index].writeRef()));
            }
        }

        rhi::ComPtr<rhi::ITexture> luminance;
        rhi::ComPtr<rhi::ITextureView> luminance_view;
        rhi::ComPtr<rhi::ITexture> exposure;
        rhi::ComPtr<rhi::ITextureView> exposure_view;
        rhi::ComPtr<rhi::IBuffer> histogram;
        rhi::ComPtr<rhi::IBuffer> target_ev;
        std::array<rhi::ComPtr<rhi::IShaderObject>, 4> histogram_roots{};
        rhi::ComPtr<rhi::IShaderObject> auto_exposure_root;
        if (m_settings.m_auto_exposure) {
            auto luminance_desc = spatial_desc;
            luminance_desc.size = {.width = kLuminanceExtent, .height = kLuminanceExtent, .depth = 1u};
            luminance_desc.format = rhi::Format::R32Float;
            luminance_desc.label = "luminance downsample";
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(luminance_desc, nullptr, luminance.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, luminance->getDefaultView(luminance_view.writeRef()));

            auto exposure_desc = spatial_desc;
            exposure_desc.size = {.width = 1u, .height = 1u, .depth = 1u};
            exposure_desc.format = rhi::Format::R32Float;
            exposure_desc.label = "adapted exposure offset";
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(exposure_desc, nullptr, exposure.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, exposure->getDefaultView(exposure_view.writeRef()));

            // Per-slot write-then-read buffers, matching the bloom/spatial
            // cohorts so no in-flight submission observes another frame's bytes.
            const auto device_buffer = [&](const u64 bytes, const char *const label,
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
                PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createBuffer(description, nullptr, out.writeRef()));
                return {};
            };
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device_buffer(kHistogramBins * sizeof(u32), "luminance histogram", histogram));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device_buffer(sizeof(float), "histogram target EV", target_ev));

            for (u32 index = 0u; index < histogram_roots.size(); ++index) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RetroPass,
                    device.createRootShaderObject(m_histogram_programs[index], histogram_roots[index].writeRef()));
            }
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device.createRootShaderObject(m_auto_exposure_program, auto_exposure_root.writeRef()));
        }

        // The caller proved completion of this slot. Fresh roots detach old
        // bindings too; all acquisition succeeds before replacing its cohort.
        rhi::ComPtr<rhi::IShaderObject> present_root, spatial_root;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createRootShaderObject(m_program, present_root.writeRef()));
        if (m_settings.m_spatial_enabled or m_settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device.createRootShaderObject(m_spatial_program, spatial_root.writeRef()));
        }

        slot.m_history = std::move(history);
        slot.m_history_view = std::move(history_view);
        slot.m_temporal = m_settings.m_temporal_enabled;
        slot.m_temporal_2x = m_settings.m_temporal_2x_enabled;
        slot.m_reactive = std::move(reactive);
        slot.m_reactive_view = std::move(reactive_view);
        m_temporal_2x_roots[m_scene_cursor] = std::move(temporal_2x_root);
        slot.m_color = std::move(color);
        slot.m_color_view = std::move(color_view);
        slot.m_depth = std::move(depth);
        slot.m_depth_view = std::move(depth_view);
        slot.m_extent = extent;
        slot.m_output_extent = m_output_extent;
        slot.m_spatial_enabled = m_settings.m_spatial_enabled;
        slot.m_spatial = std::move(spatial);
        slot.m_spatial_view = std::move(spatial_view);
        slot.m_bloom_enabled = m_settings.m_bloom_enabled;
        slot.m_bloom = std::move(bloom);
        slot.m_bloom_view = std::move(bloom_view);
        slot.m_bloom_uav_views = std::move(bloom_uav_views);
        slot.m_auto_exposure_enabled = m_settings.m_auto_exposure;
        slot.m_luminance = std::move(luminance);
        slot.m_luminance_view = std::move(luminance_view);
        slot.m_exposure = std::move(exposure);
        slot.m_exposure_view = std::move(exposure_view);
        slot.m_histogram = std::move(histogram);
        slot.m_target_ev = std::move(target_ev);
        m_bloom_roots[m_scene_cursor] = std::move(bloom_roots);
        m_histogram_roots[m_scene_cursor] = std::move(histogram_roots);
        m_auto_exposure_roots[m_scene_cursor] = std::move(auto_exposure_root);
        m_scene_roots[m_scene_cursor] = std::move(present_root);
        m_spatial_roots[m_scene_cursor] = std::move(spatial_root);
        // A completed legacy temporal root can retain the previous MSAA views.
        if (not m_settings.m_temporal_enabled) {
            m_temporal_roots[m_scene_cursor].setNull();
        }

        return default_value_v;
    }

    std::error_code RetroPass::enableCompletedPictureRetention() {
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            not m_settings.m_temporal_enabled ? std::error_code{} : std::make_error_code(std::errc::not_supported));
        m_retain_completed_picture = true;
        return {};
    }

    std::optional<RetroCompletedPicture> RetroPass::restoreCompletedPicture(const FenceCompletionValue completed) {
        if (not m_retain_completed_picture) {
            return std::nullopt;
        }

        // An abandoned scene may have prepared/rendered CPU state but no
        // accepted write. A UI-only submission must never publish that state.
        m_scene_ready = false;
        m_scene_recorded = false;
        m_scene_sampled = false;
        m_history_used = false;

        for (u32 index = 0u; index < kFrameRingSlots; ++index) {
            const auto value = m_scene_slots[index].m_content_submit;
            if (value != kFenceValueNone and
                slotFenceComplete(completed, value) and
                (not m_completed_picture_valid or value > m_scene_slots[m_completed_picture_slot].m_content_submit)) {
                m_completed_picture_slot = index;
                m_completed_picture_valid = true;
            }
        }

        if (not m_completed_picture_valid) {
            return std::nullopt;
        }

        const auto &slot = m_scene_slots[m_completed_picture_slot];
        m_scene_slot = m_completed_picture_slot;
        m_scene_ready = true;
        m_scene_recorded = false;
        m_history_used = false;
        m_settings = slot.m_picture_settings;
        m_camera = slot.m_picture_camera;
        m_content_identity = slot.m_picture_identity;
        m_content_lighting_revision = slot.m_picture_lighting_revision;
        m_lighting_publication = slot.m_picture_lighting_publication;
        m_request_elapsed = slot.m_picture_request_elapsed;
        m_frame_extent = slot.m_extent;
        m_output_extent = slot.m_output_extent;
        return RetroCompletedPicture{
            slot.m_picture_camera, slot.m_picture_identity, slot.m_picture_request_elapsed, slot.m_output_extent,
            slot.m_picture_lighting_revision, slot.m_picture_lighting_publication};
    }

    std::error_code RetroPass::prepareScene(
        const PreRenderContext &context, const RenderPipelineSignature &scene_signature, const int2 extent,
        const DrawCallback encode_scene) {
        const hal::ProfileScope scope{"App.Pass.Retro.Scene"};

        const bool previous_ready = m_scene_ready;
        const u32 previous_slot = m_scene_slot;
        const bool previous_recorded = m_scene_recorded;
        bool prepared{};
        PPR_DEFER {
            if (m_retain_completed_picture and not prepared) {
                m_scene_ready = previous_ready;
                m_scene_slot = previous_slot;
                m_scene_recorded = previous_recorded;
            }
        };

        m_scene_ready = false;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_ready ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            extent.x > 0 and extent.y > 0 ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, scene_signature == (m_settings.m_temporal_2x_enabled
                                                  ? temporal2xScenePipelineSignature()
                                                  : scenePipelineSignature(
                                                        m_settings.m_temporal_enabled or m_settings.m_spatial_enabled))
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, not m_retain_completed_picture or not m_settings.m_temporal_enabled
                           ? std::error_code{}
                           : std::make_error_code(std::errc::not_supported));

        if (m_settings.m_spatial_enabled or m_settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, validateFrameSettings(m_settings, extent, m_output_extent));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, ensureSpatialProgram_(context.m_device));
        }

        if (m_settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, ensureTemporal2xProgram_(context.m_device));
        }

        if (m_settings.m_bloom_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, ensureBloomPrograms_(context.m_device));
        }

        if (m_settings.m_auto_exposure) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, ensureHistogramPrograms_(context.m_device));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, ensureAutoExposureProgram_(context.m_device));
        }

        std::array<FenceCompletionValue, kFrameRingSlots> values;
        for (u32 slot = 0u; slot < kFrameRingSlots; ++slot) {
            values[slot] = m_scene_slots[slot].m_last_use;
        }

        u32 selected = selectRingSlot(context.m_completed_fence_value, values, m_scene_cursor);
        if (m_retain_completed_picture) {
            // Promote completed writes without restoring old display settings.
            for (u32 index = 0u; index < kFrameRingSlots; ++index) {
                const auto value = m_scene_slots[index].m_content_submit;
                if (value != kFenceValueNone and
                    slotFenceComplete(context.m_completed_fence_value, value) and
                    (not m_completed_picture_valid or
                        value > m_scene_slots[m_completed_picture_slot].m_content_submit)) {
                    m_completed_picture_slot = index;
                    m_completed_picture_valid = true;
                }
            }

            if (m_completed_picture_valid and selected == m_completed_picture_slot) {
                selected = (selected + 1u) % kFrameRingSlots;
            }
        }

        // Exclude both protected owners in the same ring walk: sequential
        // skips can wrap back onto the completed picture.
        for (u32 attempt = 0u; attempt < kFrameRingSlots; ++attempt) {
            const bool protected_picture = m_retain_completed_picture and
                                           m_completed_picture_valid and
                                           selected == m_completed_picture_slot;
            const bool protected_history = m_history_valid and selected == m_history_slot;
            if (not protected_picture and not protected_history) {
                break;
            }

            selected = (selected + 1u) % kFrameRingSlots;
        }

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
        if (not slot.m_color or
            slot.m_extent.x != extent.x or
            slot.m_extent.y != extent.y or
            slot.m_temporal != m_settings.m_temporal_enabled or
            slot.m_temporal_2x != m_settings.m_temporal_2x_enabled or
            slot.m_spatial_enabled != m_settings.m_spatial_enabled or
            slot.m_bloom_enabled != m_settings.m_bloom_enabled or
            slot.m_auto_exposure_enabled != m_settings.m_auto_exposure or
            slot.m_output_extent.x != m_output_extent.x or
            slot.m_output_extent.y != m_output_extent.y) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, createSceneTarget_(context.m_device, slot, extent));
        }

        if (m_settings.m_temporal_enabled and not m_temporal_roots[selected]) {
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass,
                context.m_device.createRootShaderObject(m_temporal_program, m_temporal_roots[selected].writeRef()));
        }

        const rhi::SubresourceRange target_range{
            .layer = 0u,
            .layerCount = 1u,
            .mip = 0u,
            .mipCount = 1u,
        };
        slot.m_content_submit = kFenceValueNone;
        context.m_encoder.setTextureState(slot.m_color, target_range, rhi::ResourceState::RenderTarget);
        context.m_encoder.setTextureState(slot.m_depth, target_range, rhi::ResourceState::DepthWrite);
        if (m_settings.m_temporal_2x_enabled) {
            context.m_encoder.setTextureState(slot.m_reactive, target_range, rhi::ResourceState::RenderTarget);
        }

        const std::array<rhi::RenderPassColorAttachment, 2> color_attachments{
            rhi::RenderPassColorAttachment{.view = slot.m_color_view, .clearValue = {0.1f, 0.1f, 0.2f, 1.0f}},
            rhi::RenderPassColorAttachment{.view = slot.m_reactive_view, .clearValue = {0.0f, 0.0f, 0.0f, 0.0f}},
        };
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = slot.m_depth_view;
        depth_attachment.depthClearValue = 1.0f;
        rhi::RenderPassDesc pass_desc{};
        pass_desc.colorAttachments = color_attachments.data();
        pass_desc.colorAttachmentCount = m_settings.m_temporal_2x_enabled ? 2u : 1u;
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

            pass->setRenderState({
                .viewports = {scene_context.m_viewport},
                .viewportCount = 1u,
                .scissorRects = {scene_context.m_scissor},
                .scissorRectCount = 1u,
                .vertexBuffers = {},
                .indexBuffer = {},
            });
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, encode_scene(scene_context));
        }

        context.m_encoder.setTextureState(slot.m_color, target_range, rhi::ResourceState::ShaderResource);

        if (m_settings.m_spatial_enabled or m_settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, resolveSpatial_(context, selected));
        } else if (m_settings.m_temporal_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, resolveTemporal_(context, selected));
        }

        if (m_settings.m_temporal_2x_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, resolveTemporal2x_(context, selected));
        }

        if (m_settings.m_auto_exposure) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, resolveAdaptation_(context, selected));
        }

        if (m_settings.m_bloom_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, resolveBloom_(context, selected));
        }

        m_scene_ready = true;
        m_scene_slot = selected;
        m_scene_recorded = true;
        slot.m_picture_camera = m_camera;
        slot.m_picture_identity = m_content_identity;
        slot.m_picture_lighting_revision = m_content_lighting_revision;
        slot.m_picture_lighting_publication = m_lighting_publication;
        slot.m_picture_request_elapsed = m_request_elapsed;
        slot.m_picture_settings = m_settings;
        prepared = true;
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // current-frame spatial reconstruction
    // ------------------------------------------------------------------

    std::error_code RetroPass::ensureSpatialProgram_(rhi::IDevice &device) {
        if (m_spatial_pipeline) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_shader_service.isValid() ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            m_shader_service->loadModuleFromFile(
                m_content_dir / TEXT("shaders") / TEXT("spatial_msaa4.slang"), "spatial_msaa4", module.writeRef()));

        auto *const layout = module->getLayout();
        slang::TypeLayoutReflection *parameters = nullptr;
        if (layout != nullptr) {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == "g_spatial") {
                    parameters = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, parameters != nullptr and parameters->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 3>{
                 std::pair{"source_extent", 0u}, {"output_extent", 8u}, {"sample_indices", 16u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < parameters->getFieldCount(); ++index) {
                auto *const field = parameters->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    matched = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, matched ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, module->findEntryPointByName("resolveSpatialMsaa4", entry.writeRef()));
        slang::IComponentType *entries[] = {entry.get()};
        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = module.get();
        description.slangEntryPoints = entries;
        description.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics;
        rhi::ComPtr<rhi::IShaderProgram> program;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, device.createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));

        rhi::ComputePipelineDesc pipeline_desc{};
        pipeline_desc.program = program;
        pipeline_desc.label = "Hable current-frame spatial reconstruction";
        rhi::ComPtr<rhi::IComputePipeline> pipeline;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createComputePipeline(pipeline_desc, pipeline.writeRef()));
        std::array<rhi::ComPtr<rhi::IShaderObject>, kFrameRingSlots> roots{};
        for (auto &root: roots) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createRootShaderObject(program, root.writeRef()));
        }

        m_spatial_program = std::move(program);
        m_spatial_pipeline = std::move(pipeline);
        m_spatial_roots = std::move(roots);
        return {};
    }

    std::error_code RetroPass::resolveSpatial_(const PreRenderContext &context, const u32 selected) {
        const hal::ProfileScope scope{"App.Pass.Spatial.Resolve"};
        struct alignas(16) Parameters {
            u32 m_source[2];
            u32 m_output[2];
            u32 m_samples[4];
        };
        static_assert(sizeof(Parameters) == 32u);
        static_assert(alignof(Parameters) == 16u);
        static_assert(PPR_OFFSETOF(Parameters, m_output) == 8u);
        static_assert(PPR_OFFSETOF(Parameters, m_samples) == 16u);

        auto &slot = m_scene_slots[selected];
        const Parameters parameters{
            {safe_narrowing(slot.m_extent.x), safe_narrowing(slot.m_extent.y)},
            {safe_narrowing(slot.m_output_extent.x), safe_narrowing(slot.m_output_extent.y)},
            // Vulkan's admitted standard indexed table matches this canonical order.
            {0u, 1u, 2u, 3u},
        };
        rhi::ShaderCursor cursor{m_spatial_roots[selected]};
        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_spatial"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, constants.setData(&parameters, sizeof(parameters)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_source"].setBinding(rhi::Binding(slot.m_color_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_output"].setBinding(rhi::Binding(slot.m_spatial_view)));

        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        context.m_encoder.setTextureState(slot.m_spatial, range, rhi::ResourceState::UnorderedAccess);
        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        pass->bindPipeline(m_spatial_pipeline, m_spatial_roots[selected]);
        pass->dispatchCompute((parameters.m_source[0] + 7u) / 8u, (parameters.m_source[1] + 7u) / 8u, 1u);
        pass->end();

        context.m_encoder.setTextureState(slot.m_spatial, range, rhi::ResourceState::ShaderResource);
        return {};
    }

    // ------------------------------------------------------------------
    // output-resolution temporal reconstruction with reactive support
    // ------------------------------------------------------------------

    std::error_code RetroPass::ensureTemporal2xProgram_(rhi::IDevice &device) {
        if (m_temporal_2x_pipeline) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_shader_service.isValid() ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            m_shader_service->loadModuleFromFile(
                m_content_dir / TEXT("shaders") / TEXT("temporal_2x.slang"), "temporal_2x", module.writeRef()));

        slang::TypeLayoutReflection *parameters = nullptr;
        auto *const layout = module->getLayout();
        if (layout != nullptr) {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == "g_tsr") {
                    parameters = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, parameters != nullptr and parameters->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 192u
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 10>{
                 std::pair{"inverse_view_projection", 0u}, {"previous_view_projection", 64u}, {"sample_offsets", 128u},
                 {"raster_extent", 144u}, {"output_extent", 152u}, {"sample_indices", 160u}, {"flags", 176u},
                 {"reserved0", 180u}, {"reserved1", 184u}, {"reserved2", 188u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < parameters->getFieldCount(); ++index) {
                auto *const field = parameters->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    matched = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, matched ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, module->findEntryPointByName("resolveTemporal2x", entry.writeRef()));
        slang::IComponentType *entries[] = {entry.get()};
        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = module.get();
        description.slangEntryPoints = entries;
        description.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics;
        rhi::ComPtr<rhi::IShaderProgram> program;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, device.createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));

        rhi::ComputePipelineDesc pipeline_desc{};
        pipeline_desc.program = program;
        pipeline_desc.label = "temporal2x scene-linear reconstruction";
        rhi::ComPtr<rhi::IComputePipeline> pipeline;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createComputePipeline(pipeline_desc, pipeline.writeRef()));

        m_temporal_2x_program = std::move(program);
        m_temporal_2x_pipeline = std::move(pipeline);
        return {};
    }

    std::error_code RetroPass::resolveTemporal2x_(const PreRenderContext &context, const u32 selected) {
        const hal::ProfileScope scope{"App.Pass.Temporal2x.Resolve"};
        struct alignas(16) Parameters {
            float4x4 m_inverse_view_projection{identity_v};
            float4x4 m_previous_view_projection{identity_v};
            float m_sample_offsets[4]{};
            u32 m_raster_extent[2]{};
            u32 m_output_extent[2]{};
            u32 m_sample_indices[4]{0u, 1u, 2u, 3u};
            u32 m_flags{};
            u32 m_reserved0{}, m_reserved1{}, m_reserved2{};
        };
        static_assert(sizeof(Parameters) == 192u and alignof(Parameters) == 16u);
        static_assert(PPR_OFFSETOF(Parameters, m_sample_offsets) == 128u);
        static_assert(PPR_OFFSETOF(Parameters, m_raster_extent) == 144u);
        static_assert(PPR_OFFSETOF(Parameters, m_output_extent) == 152u);
        static_assert(PPR_OFFSETOF(Parameters, m_sample_indices) == 160u);
        static_assert(PPR_OFFSETOF(Parameters, m_flags) == 176u);
        static_assert(PPR_OFFSETOF(Parameters, m_reserved0) == 180u);
        static_assert(PPR_OFFSETOF(Parameters, m_reserved1) == 184u);
        static_assert(PPR_OFFSETOF(Parameters, m_reserved2) == 188u);

        auto &slot = m_scene_slots[selected];
        const auto &history = m_scene_slots[m_history_slot];
        const bool history_valid = m_temporal_2x_history_eligible;
        Parameters parameters{};
        parameters.m_inverse_view_projection = m_camera.m_invert_view_projection;
        parameters.m_previous_view_projection = history_valid ? history.m_picture_camera.m_view_projection
                                                              : m_camera.m_view_projection;
        const auto current_jitter = temporalJitter(m_frame_phase);
        const auto previous_jitter = temporalJitter((m_frame_phase + 3u) % 4u);
        parameters.m_sample_offsets[0] = -current_jitter.x / float(slot.m_extent.x);
        parameters.m_sample_offsets[1] = current_jitter.y / float(slot.m_extent.y);
        parameters.m_sample_offsets[2] = -previous_jitter.x / float(slot.m_extent.x);
        parameters.m_sample_offsets[3] = previous_jitter.y / float(slot.m_extent.y);
        parameters.m_raster_extent[0] = safe_narrowing(slot.m_extent.x);
        parameters.m_raster_extent[1] = safe_narrowing(slot.m_extent.y);
        parameters.m_output_extent[0] = safe_narrowing(slot.m_output_extent.x);
        parameters.m_output_extent[1] = safe_narrowing(slot.m_output_extent.y);
        parameters.m_flags = history_valid ? 1u : 0u;
        if (m_camera.m_camera_mode == ECameraProjection::perspective) {
            parameters.m_flags |= 2u;
        }

        bool stationary = true;
        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 column = 0u; column < 4u; ++column) {
                stationary = stationary and m_camera.m_view_projection[row][column] ==
                                                parameters.m_previous_view_projection[row][column];
            }
        }

        if (stationary) {
            parameters.m_flags |= 4u;
        }

        rhi::ShaderCursor cursor{m_temporal_2x_roots[selected]};
        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_tsr"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, constants.setData(&parameters, sizeof(parameters)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_current"].setBinding(rhi::Binding(slot.m_spatial_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_raw"].setBinding(rhi::Binding(slot.m_color_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_depth"].setBinding(rhi::Binding(slot.m_depth_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_reactive"].setBinding(rhi::Binding(slot.m_reactive_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, cursor["g_history"].setBinding(
                           rhi::Binding(history_valid ? history.m_history_view : m_empty_history_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_output"].setBinding(rhi::Binding(slot.m_history_view)));

        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        context.m_encoder.setTextureState(slot.m_depth, range, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(slot.m_reactive, range, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(slot.m_history, range, rhi::ResourceState::UnorderedAccess);
        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        pass->bindPipeline(m_temporal_2x_pipeline, m_temporal_2x_roots[selected]);
        pass->dispatchCompute((parameters.m_output_extent[0] + 7u) / 8u, (parameters.m_output_extent[1] + 7u) / 8u, 1u);
        pass->end();

        context.m_encoder.setTextureState(slot.m_history, range, rhi::ResourceState::ShaderResource);
        m_history_used = history_valid;
        return {};
    }

    // ------------------------------------------------------------------
    // exposure-relative scene-linear bloom
    // ------------------------------------------------------------------

    std::error_code RetroPass::ensureBloomPrograms_(rhi::IDevice &device) {
        if (m_bloom_pipelines[0]) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_shader_service.isValid() ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_shader_service->loadModuleFromFile(
                           m_content_dir / TEXT("shaders") / TEXT("spd_bloom.slang"), "spd_bloom", module.writeRef()));

        auto *const layout = module->getLayout();
        slang::TypeLayoutReflection *parameters = nullptr;
        if (layout != nullptr) {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == "g_bloom") {
                    parameters = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            parameters != nullptr and parameters->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(BloomParameters)
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 6>{
                 std::pair{"sourceExtent", 0u}, {"mip0Extent", 8u}, {"threshold", 16u}, {"softKnee", 20u},
                 {"mipCount", 24u}, {"upsampleLevel", 28u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < parameters->getFieldCount(); ++index) {
                auto *const field = parameters->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    matched = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, matched ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        std::array<rhi::ComPtr<rhi::IShaderProgram>, 3> programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 3> pipelines{};
        const char *const names[] = {"prefilterBloom", "downsampleBloom", "upsampleBloom"};
        for (u32 index = 0u; index < programs.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, module->findEntryPointByName(names[index], entry.writeRef()));
            slang::IComponentType *entries[] = {entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device.createShaderProgram(description, programs[index].writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline_desc{};
            pipeline_desc.program = programs[index];
            pipeline_desc.label = names[index];
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device.createComputePipeline(pipeline_desc, pipelines[index].writeRef()));
        }

        m_bloom_programs = std::move(programs);
        m_bloom_pipelines = std::move(pipelines);
        return {};
    }

    std::error_code RetroPass::resolveBloom_(const PreRenderContext &context, const u32 selected) {
        const hal::ProfileScope scope{"App.Pass.Retro.Bloom"};
        auto &slot = m_scene_slots[selected];

        const int2 mip0 = bloomMip0Extent(slot.m_output_extent);
        const u32 mip_count = bloomMipCount(mip0);
        BloomParameters parameters{};
        parameters.m_source_extent[0] = safe_narrowing(slot.m_output_extent.x);
        parameters.m_source_extent[1] = safe_narrowing(slot.m_output_extent.y);
        parameters.m_mip0_extent[0] = safe_narrowing(mip0.x);
        parameters.m_mip0_extent[1] = safe_narrowing(mip0.y);
        parameters.m_threshold = m_settings.m_bloom_threshold;
        parameters.m_soft_knee = kBloomSoftKnee;
        parameters.m_mip_count = mip_count;
        parameters.m_upsample_level = 0u;

        static constexpr std::array<const char *, kMaxBloomMips> kBloomMipNames{
            "g_bloom_mip0", "g_bloom_mip1", "g_bloom_mip2", "g_bloom_mip3", "g_bloom_mip4", "g_bloom_mip5"};
        const rhi::Binding source_scene(slot.m_temporal_2x ? slot.m_history_view : slot.m_spatial_view);

        // Prefilter constants and bindings: the resolved scene in, mip 0 out.
        {
            rhi::ShaderCursor cursor{m_bloom_roots[selected][0]};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_bloom"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, constants.setData(&parameters, sizeof(parameters)));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_scene"].setBinding(source_scene));
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, cursor["g_bloom_mip0"].setBinding(rhi::Binding(slot.m_bloom_uav_views[0])));
        }

        // Downsample constants and the full per-mip UAV set. Mips beyond the
        // logical chain depth are never accessed by the shader (their guards are
        // false), so they alias the last valid view to keep every binding valid.
        {
            rhi::ShaderCursor cursor{m_bloom_roots[selected][1]};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_bloom"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, constants.setData(&parameters, sizeof(parameters)));
            for (u32 level = 0u; level < kMaxBloomMips; ++level) {
                const u32 view = std::min(level, mip_count - 1u);
                PPR_RETURN_ERROR_ON_FAIL(
                    RetroPass, cursor[kBloomMipNames[level]].setBinding(rhi::Binding(slot.m_bloom_uav_views[view])));
            }
        }

        // Upsample constants; its source/destination views are rebound per level.
        rhi::ShaderCursor upsample_cursor{m_bloom_roots[selected][2]};
        rhi::ShaderCursor upsample_constants;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, upsample_cursor["g_bloom"].getDereferenced(upsample_constants));

        const rhi::SubresourceRange chain_range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = mip_count};
        context.m_encoder.setTextureState(slot.m_bloom, chain_range, rhi::ResourceState::UnorderedAccess);

        const auto dispatch = [&](const u32 program, const u32 x, const u32 y) -> std::error_code {
            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->bindPipeline(m_bloom_pipelines[program], m_bloom_roots[selected][program]);
            pass->dispatchCompute(x, y, 1u);
            pass->end();
            return {};
        };

        // Prefilter to mip 0, then the single-pass downsample for mips 1..5.
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, dispatch(0u, (checked_cast<u32>(mip0.x) + 7u) / 8u, (checked_cast<u32>(mip0.y) + 7u) / 8u));
        context.m_encoder.globalBarrier();
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, dispatch(1u, (checked_cast<u32>(mip0.x) + 31u) / 32u, (checked_cast<u32>(mip0.y) + 31u) / 32u));

        // Progressive tent upsample: accumulate mip K back into mip K-1, coarse to fine.
        for (u32 level = mip_count - 1u; level >= 1u; --level) {
            const int2 dst_extent = bloomMipExtent(mip0, level - 1u);
            parameters.m_upsample_level = level;
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, upsample_constants.setData(&parameters, sizeof(parameters)));
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, upsample_cursor["g_up_src"].setBinding(rhi::Binding(slot.m_bloom_uav_views[level])));
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, upsample_cursor["g_up_dst"].setBinding(rhi::Binding(slot.m_bloom_uav_views[level - 1u])));

            context.m_encoder.globalBarrier();
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass,
                dispatch(2u, (checked_cast<u32>(dst_extent.x) + 7u) / 8u, (checked_cast<u32>(dst_extent.y) + 7u) / 8u));
        }

        context.m_encoder.setTextureState(slot.m_bloom, chain_range, rhi::ResourceState::ShaderResource);
        return {};
    }

    // ------------------------------------------------------------------
    // histogram eye adaptation
    // ------------------------------------------------------------------

    std::error_code RetroPass::ensureHistogramPrograms_(rhi::IDevice &device) {
        if (m_histogram_pipelines[0]) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_shader_service.isValid() ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_shader_service->loadModuleFromFile(
                           m_content_dir / TEXT("shaders") / TEXT("luminance_histogram.slang"), "luminance_histogram",
                           module.writeRef()));

        auto *const layout = module->getLayout();
        slang::TypeLayoutReflection *parameters = nullptr;
        if (layout != nullptr) {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == "g_luminance") {
                    parameters = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, parameters != nullptr and parameters->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) ==
                                                     sizeof(LuminanceHistogramParameters)
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 4>{
                 std::pair{"sourceExtent", 0u}, {"luminanceExtent", 8u}, {"reserved0", 16u}, {"reserved1", 20u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < parameters->getFieldCount(); ++index) {
                auto *const field = parameters->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    matched = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, matched ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        std::array<rhi::ComPtr<rhi::IShaderProgram>, 4> programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 4> pipelines{};
        const char *const names[] = {"clearHistogram", "computeLuminance", "buildHistogram", "reduceHistogram"};
        for (u32 index = 0u; index < programs.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, module->findEntryPointByName(names[index], entry.writeRef()));
            slang::IComponentType *entries[] = {entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device.createShaderProgram(description, programs[index].writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline_desc{};
            pipeline_desc.program = programs[index];
            pipeline_desc.label = names[index];
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, device.createComputePipeline(pipeline_desc, pipelines[index].writeRef()));
        }

        m_histogram_programs = std::move(programs);
        m_histogram_pipelines = std::move(pipelines);
        return {};
    }

    std::error_code RetroPass::ensureAutoExposureProgram_(rhi::IDevice &device) {
        if (m_auto_exposure_pipeline) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, m_shader_service.isValid() ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            m_shader_service->loadModuleFromFile(
                m_content_dir / TEXT("shaders") / TEXT("auto_exposure.slang"), "auto_exposure", module.writeRef()));

        auto *const layout = module->getLayout();
        slang::TypeLayoutReflection *parameters = nullptr;
        if (layout != nullptr) {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == "g_auto_exposure") {
                    parameters = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, parameters != nullptr and
                               parameters->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(AutoExposureParameters)
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 4>{
                 std::pair{"adaptationAlpha", 0u}, {"evMin", 4u}, {"evMax", 8u}, {"enabled", 12u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < parameters->getFieldCount(); ++index) {
                auto *const field = parameters->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    matched = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, matched ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        shader::ComPtr<slang::IEntryPoint> entry;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, module->findEntryPointByName("adaptExposure", entry.writeRef()));
        slang::IComponentType *entries[] = {entry.get()};
        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = module.get();
        description.slangEntryPoints = entries;
        description.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            device.createShaderProgram(description, m_auto_exposure_program.writeRef(), diagnostics.writeRef()));

        rhi::ComputePipelineDesc pipeline_desc{};
        pipeline_desc.program = m_auto_exposure_program;
        pipeline_desc.label = "histogram eye-adaptation temporal state";
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, device.createComputePipeline(pipeline_desc, m_auto_exposure_pipeline.writeRef()));

        return {};
    }

    std::error_code RetroPass::resolveAdaptation_(const PreRenderContext &context, const u32 selected) {
        const hal::ProfileScope scope{"App.Pass.Retro.Adaptation"};
        auto &slot = m_scene_slots[selected];
        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};

        const LuminanceHistogramParameters histogram_parameters{
            {safe_narrowing(slot.m_output_extent.x), safe_narrowing(slot.m_output_extent.y)},
            {kLuminanceExtent, kLuminanceExtent},
            0u,
            0u,
        };

        // The resolved temporal2x scene is the histogram source.
        const rhi::Binding source_scene(slot.m_history_view);
        const rhi::Binding luminance_view(slot.m_luminance_view);
        const auto histogram_range = [&](rhi::IBuffer *const buffer) {
            return rhi::BufferRange{.offset = 0u, .size = buffer->getDesc().size};
        };

        const auto dispatch = [&](const u32 index, const u32 x, const u32 y, const u32 z) -> std::error_code {
            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            pass->bindPipeline(m_histogram_pipelines[index], m_histogram_roots[selected][index]);
            pass->dispatchCompute(x, y, z);
            pass->end();
            return {};
        };

        // Shared constants and static bindings on every histogram root.
        for (u32 index = 0u; index < m_histogram_roots[selected].size(); ++index) {
            rhi::ShaderCursor cursor{m_histogram_roots[selected][index]};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_luminance"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, constants.setData(&histogram_parameters, sizeof(histogram_parameters)));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_scene"].setBinding(source_scene));
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_luminance_image"].setBinding(luminance_view));
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass,
                cursor["g_histogram"].setBinding(rhi::Binding(slot.m_histogram, histogram_range(slot.m_histogram))));
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass,
                cursor["g_target_ev"].setBinding(rhi::Binding(slot.m_target_ev, histogram_range(slot.m_target_ev))));
        }

        context.m_encoder.setTextureState(slot.m_luminance, range, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setBufferState(slot.m_histogram, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.setBufferState(slot.m_target_ev, rhi::ResourceState::UnorderedAccess);

        PPR_RETURN_ERROR_ON_FAIL(RetroPass, dispatch(0u, 1u, 1u, 1u)); // clearHistogram
        context.m_encoder.globalBarrier();
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, dispatch(1u, kLuminanceExtent / 8u, kLuminanceExtent / 8u, 1u)); // computeLuminance
        context.m_encoder.globalBarrier();
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, dispatch(2u, kLuminanceExtent / 8u, kLuminanceExtent / 8u, 1u)); // buildHistogram
        context.m_encoder.globalBarrier();
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, dispatch(3u, 1u, 1u, 1u)); // reduceHistogram
        context.m_encoder.globalBarrier();

        const AutoExposureParameters auto_parameters{
            .m_adaptation_alpha = adaptationAlpha(m_frame_delta, m_settings.m_adaptation_tau),
            .m_ev_min = m_settings.m_ev_min,
            .m_ev_max = m_settings.m_ev_max,
            .m_enabled = 1u,
        };

        const auto &history_slot = m_scene_slots[m_history_slot];
        const bool history_valid = m_history_valid and
                                   history_slot.m_auto_exposure_enabled and
                                   history_slot.m_exposure_view != nullptr;
        const auto previous_exposure = history_valid ? history_slot.m_exposure_view : m_empty_exposure_view;

        rhi::ShaderCursor cursor{m_auto_exposure_roots[selected]};
        rhi::ShaderCursor constants;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_auto_exposure"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, constants.setData(&auto_parameters, sizeof(auto_parameters)));
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            cursor["g_target_ev"].setBinding(rhi::Binding(slot.m_target_ev, histogram_range(slot.m_target_ev))));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_previous_exposure"].setBinding(rhi::Binding(previous_exposure)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_exposure"].setBinding(rhi::Binding(slot.m_exposure_view)));

        context.m_encoder.setTextureState(slot.m_exposure, range, rhi::ResourceState::UnorderedAccess);
        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        pass->bindPipeline(m_auto_exposure_pipeline, m_auto_exposure_roots[selected]);
        pass->dispatchCompute(1u, 1u, 1u);
        pass->end();

        context.m_encoder.setTextureState(slot.m_exposure, range, rhi::ResourceState::ShaderResource);
        context.m_encoder.setTextureState(slot.m_luminance, range, rhi::ResourceState::ShaderResource);
        return {};
    }

    // ------------------------------------------------------------------
    // multisample temporal reconstruction
    // ------------------------------------------------------------------

    std::error_code RetroPass::createTemporalProgram_(
        IShaderService &shader_service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule module{};
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass,
            shader_service.loadModuleFromFile(
                content_dir / TEXT("shaders") / TEXT("temporal_resolve.slang"), "temporal_resolve", module.writeRef()));

        shader::ComPtr<slang::IEntryPoint> vertex, fragment;
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, module->findEntryPointByName("vertexTemporal", vertex.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, module->findEntryPointByName("fragmentTemporal", fragment.writeRef()));

        slang::IComponentType *entries[] = {vertex.get(), fragment.get()};
        rhi::ShaderProgramDesc program{};
        program.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program.slangGlobalScope = module.get();
        program.slangEntryPoints = entries;
        program.slangEntryPointCount = 2u;
        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, device.createShaderProgram(program, m_temporal_program.writeRef(), diagnostics.writeRef()));

        for (auto &root: m_temporal_roots) {
            PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createRootShaderObject(m_temporal_program, root.writeRef()));
        }

        const float empty[4]{};
        const rhi::SubresourceData initial{.data = empty, .rowPitch = sizeof(empty), .slicePitch = sizeof(empty)};
        rhi::TextureDesc texture{};
        texture.size = {1u, 1u, 1u};
        texture.format = rhi::Format::RGBA32Float;
        texture.usage = rhi::TextureUsage::ShaderResource;
        texture.defaultState = rhi::ResourceState::ShaderResource;
        texture.label = "temporal empty history";
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createTexture(texture, &initial, m_empty_history.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, m_empty_history->getDefaultView(m_empty_history_view.writeRef()));

        rhi::ColorTargetDesc target{};
        target.format = rhi::Format::RGBA16Float;
        rhi::RenderPipelineDesc pipeline{};
        pipeline.program = m_temporal_program;
        pipeline.targets = &target;
        pipeline.targetCount = 1u;
        pipeline.rasterizer.cullMode = rhi::CullMode::None;
        pipeline.label = "temporal MSAA reconstruction";
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, device.createRenderPipeline(pipeline, m_temporal_pipeline.writeRef()));
        return {};
    }

    std::error_code RetroPass::resolveTemporal_(const PreRenderContext &context, const u32 selected) {
        const hal::ProfileScope scope{"App.Pass.Temporal.Resolve"};
        // Scalar arrays make the constant-buffer ABI independent of vector
        // constructor packing. Two 16-byte rows follow two matrices.
        struct alignas(16) TemporalParameters {
            float4x4 m_inverse_view_projection{identity_v};
            float4x4 m_previous_view_projection{identity_v};
            float m_sample_offsets[4];
            float m_extent[2];
            u32 m_flags;
            u32 m_reserved;
        };
        static_assert(sizeof(TemporalParameters) == 160u);
        static_assert(alignof(TemporalParameters) == 16u);
        static_assert(PPR_OFFSETOF(TemporalParameters, m_sample_offsets) == 128u);
        static_assert(PPR_OFFSETOF(TemporalParameters, m_extent) == 144u);
        static_assert(PPR_OFFSETOF(TemporalParameters, m_flags) == 152u);

        auto &slot = m_scene_slots[selected];
        const bool history_valid = m_history_valid and not m_scene_slots[m_history_slot].m_temporal_2x;
        const auto &history = history_valid ? m_scene_slots[m_history_slot] : slot;
        TemporalParameters parameters{};
        parameters.m_inverse_view_projection = m_camera.m_invert_view_projection;
        parameters.m_previous_view_projection = m_previous_view_projection;
        const float2 current_jitter = temporalJitter(m_frame_phase);
        const float2 previous_jitter = temporalJitter((m_frame_phase + 3u) % 4u);
        parameters.m_sample_offsets[0] = -current_jitter.x / float(slot.m_extent.x);
        parameters.m_sample_offsets[1] = current_jitter.y / float(slot.m_extent.y);
        parameters.m_sample_offsets[2] = -previous_jitter.x / float(slot.m_extent.x);
        parameters.m_sample_offsets[3] = previous_jitter.y / float(slot.m_extent.y);

        parameters.m_extent[0] = float(slot.m_extent.x);
        parameters.m_extent[1] = float(slot.m_extent.y);
        parameters.m_flags = history_valid ? 1u : 0u;
        if (m_camera.m_camera_mode == ECameraProjection::perspective) {
            parameters.m_flags |= 2u;
        }

        bool stationary = true;
        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 column = 0u; column < 4u; ++column) {
                if (m_camera.m_view_projection[row][column] != m_previous_view_projection[row][column]) {
                    stationary = false;
                }
            }
        }

        if (stationary) {
            parameters.m_flags |= 4u;
        }

        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        context.m_encoder.setTextureState(slot.m_depth, range, rhi::ResourceState::ShaderResource);
        // Bind a different, initialized texture on first frame too. The shader
        // returns before reading it, avoiding a read/write resource alias.
        const auto history_view = history_valid ? history.m_history_view : m_empty_history_view;
        context.m_encoder.setTextureState(slot.m_history, range, rhi::ResourceState::RenderTarget);

        auto *const root = m_temporal_roots[selected].get();
        rhi::ShaderCursor cursor{root};
        rhi::ShaderCursor constants{};
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_temporal"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, constants.setData(&parameters, sizeof(parameters)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_current"].setBinding(rhi::Binding(slot.m_color_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_depth"].setBinding(rhi::Binding(slot.m_depth_view)));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, cursor["g_history"].setBinding(rhi::Binding(history_view)));

        rhi::RenderPassColorAttachment attachment{};
        attachment.view = slot.m_history_view;
        rhi::RenderPassDesc desc{};
        desc.colorAttachments = &attachment;
        desc.colorAttachmentCount = 1u;
        auto *const pass = context.m_encoder.beginRenderPass(desc);
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
        pass->setRenderState({
            .viewports = {rhi::Viewport::fromSize(float(slot.m_extent.x), float(slot.m_extent.y))},
            .viewportCount = 1u,
            .scissorRects = {rhi::ScissorRect::fromSize(
                safe_narrowing(slot.m_extent.x), safe_narrowing(slot.m_extent.y))},
            .scissorRectCount = 1u,
            .vertexBuffers = {},
            .indexBuffer = {},
        });
        pass->bindPipeline(m_temporal_pipeline, root);
        pass->draw({.vertexCount = 3u});
        pass->end();

        context.m_encoder.setTextureState(slot.m_history, range, rhi::ResourceState::ShaderResource);
        m_history_used = history_valid;
        return {};
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

        // Only the opt-in HDR path owns this SDR display encoding contract.
        // Float targets retain linear values for numerical fixture readback.
        bool encode_srgb = false;
        if (m_settings.m_hdr_enabled) {
            const rhi::Format format = surface_signature.colorFormats().front();
            const bool linear_target = format == rhi::Format::RGBA16Float;
            const bool srgb_target = format == rhi::Format::RGBA8UnormSrgb or format == rhi::Format::BGRA8UnormSrgb;
            encode_srgb = format == rhi::Format::RGBA8Unorm or format == rhi::Format::BGRA8Unorm;
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, linear_target or srgb_target or encode_srgb
                               ? std::error_code{}
                               : std::make_error_code(std::errc::not_supported));
        }

        const SceneTarget &slot = m_scene_slots[m_scene_slot];
        if (slot.m_spatial_enabled or slot.m_temporal_2x) {
            const bool retained = m_retain_completed_picture and not m_scene_recorded;
            const bool native_target = context.m_target_extent.x == slot.m_output_extent.x and
                                       context.m_target_extent.y == slot.m_output_extent.y;
            const bool retained_clip =
                retained and
                context.m_target_extent.x > 0 and
                context.m_target_extent.y > 0 and
                context.m_scissor.minX == 0u and
                context.m_scissor.minY == 0u and
                context.m_scissor.maxX ==
                    static_cast<u32>(std::min(context.m_target_extent.x, slot.m_output_extent.x)) and
                context.m_scissor.maxY == static_cast<u32>(std::min(context.m_target_extent.y, slot.m_output_extent.y));

            // Retention uses the old native-pixel viewport at the real new
            // target. Its intersection scissor clips, never rescales, UVs.
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, (retained ? retained_clip : native_target) and
                                   context.m_viewport.originX == 0.0f and
                                   context.m_viewport.originY == 0.0f and
                                   context.m_viewport.extentX == float(slot.m_output_extent.x) and
                                   context.m_viewport.extentY == float(slot.m_output_extent.y)
                               ? std::error_code{}
                               : std::make_error_code(std::errc::invalid_argument));
        }

        PPR_RETURN_ERROR_ON_FAIL(RetroPass, ensurePipeline_(context.m_device, surface_signature));

        // A retained picture can be sampled on successive UI frames without
        // prepareScene selecting a new slot. Its root remains slot-owned, so
        // finish the previous reader before changing that root's constants.
        if (m_retain_completed_picture and
            not m_scene_recorded and
            not slotFenceComplete(context.m_completed_fence_value, slot.m_last_use)) {
            PPR_RETURN_ERROR_ON_FAIL(
                RetroPass, context.m_wait_for_fence ? context.m_wait_for_fence(slot.m_last_use)
                                                    : std::make_error_code(std::errc::operation_would_block));
        }

        RetroParameters parameters{};
        for (u32 index = 0u; index < kPaletteSize; ++index) {
            parameters.m_palette[index][0] = m_palette[index].x;
            parameters.m_palette[index][1] = m_palette[index].y;
            parameters.m_palette[index][2] = m_palette[index].z;
            parameters.m_palette[index][3] = m_palette[index].w;
        }

        const int2 source_extent = slot.m_spatial_enabled or slot.m_temporal_2x ? slot.m_output_extent : slot.m_extent;
        parameters.m_source_extent[0] = static_cast<float>(source_extent.x);
        parameters.m_source_extent[1] = static_cast<float>(source_extent.y);
        parameters.m_dither_strength = kDitherStrength;
        parameters.m_palette_enabled = m_settings.m_palette_enabled ? 1u : 0u;
        parameters.m_exposure_multiplier = std::exp2(m_settings.m_exposure_ev);
        parameters.m_hdr_enabled = m_settings.m_hdr_enabled ? 1u : 0u;
        parameters.m_encode_srgb = encode_srgb ? 1u : 0u;
        parameters.m_bloom_enabled = slot.m_bloom_enabled ? 1u : 0u;
        parameters.m_bloom_intensity = m_settings.m_bloom_intensity;

        rhi::IShaderObject *const root = m_scene_roots[m_scene_slot].get();
        rhi::ShaderCursor cursor{root};
        const rhi::ShaderCursor retro_field = cursor["g_retro"];
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, retro_field.isValid() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        rhi::ShaderCursor retro_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, retro_field.getDereferenced(retro_cursor));
        PPR_RETURN_ERROR_ON_FAIL(RetroPass, retro_cursor.setData(&parameters, sizeof(parameters)));
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, cursor["g_scene"].setBinding(
                           rhi::Binding(
                               slot.m_temporal_2x       ? slot.m_history_view
                               : slot.m_spatial_enabled ? slot.m_spatial_view
                               : slot.m_temporal        ? slot.m_history_view
                                                        : slot.m_color_view)));

        // The disabled branch borrows its already-bound 2D scene view; no
        // fallback image is allocated and the shader performs no bloom loads.
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, cursor["g_bloom"].setBinding(
                           rhi::Binding(
                               slot.m_bloom_enabled     ? slot.m_bloom_view
                               : slot.m_temporal_2x     ? slot.m_history_view
                               : slot.m_spatial_enabled ? slot.m_spatial_view
                               : slot.m_temporal        ? slot.m_history_view
                                                        : slot.m_color_view)));
        // Fixed exposure samples the zero offset; adapted exposure samples the
        // per-slot offset written by the current frame's resolveAdaptation_.
        PPR_RETURN_ERROR_ON_FAIL(
            RetroPass, cursor["g_exposure"].setBinding(
                           rhi::Binding(slot.m_auto_exposure_enabled ? slot.m_exposure_view : m_empty_exposure_view)));
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
        m_scene_sampled = true;
        m_sampled_slot = m_scene_slot;

        // The source target is consumed by this single draw; a later frame
        // prepares it again before it is sampled.
        m_scene_ready = false;
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // frame settlement
    // ------------------------------------------------------------------

    std::optional<RetroTemporal2xSubmission> RetroPass::lastTemporal2xSubmission() const noexcept {
        return m_last_temporal_2x_submission;
    }

    void RetroPass::notifyFrameSubmitted(const FenceCompletionValue submit_value) noexcept {
        m_last_submitted_value = submit_value;
        if (m_scene_sampled) {
            m_scene_slots[m_sampled_slot].m_last_use = submit_value;
            m_scene_sampled = false;
        }

        if (m_scene_recorded) {
            const auto &slot = m_scene_slots[m_scene_slot];
            if (slot.m_temporal_2x) {
                RetroTemporal2xSubmission observation{};
                observation.m_lighting_publication = slot.m_picture_lighting_publication;
                observation.m_submission = submit_value;
                observation.m_raster_extent = slot.m_extent;
                observation.m_output_extent = slot.m_output_extent;
                observation.m_phase = m_frame_phase;
                observation.m_history_write_slot = m_scene_slot;
                observation.m_history_bound = m_history_used;
                if (m_history_used) {
                    observation.m_history_submission = m_scene_slots[m_history_slot].m_content_submit;
                    observation.m_history_read_slot = m_history_slot;
                }

                m_last_temporal_2x_submission = observation;
            }

            m_scene_slots[m_scene_slot].m_content_submit = submit_value;
            // The frame that rendered into the slot is (or is about to be)
            // submitted under this value: the slot is reusable only once the
            // fence passes it.
            m_scene_slots[m_scene_slot].m_last_use = submit_value;
            if (m_history_used) {
                // Sampling history extends its lifetime through this reader.
                m_scene_slots[m_history_slot].m_last_use = submit_value;
            }
            m_history_slot = m_scene_slot;
            m_history_valid = m_settings.m_temporal_enabled or m_settings.m_temporal_2x_enabled;
            m_previous_view_projection = m_camera.m_view_projection;
            if (m_settings.m_temporal_2x_enabled or not m_settings.m_spatial_enabled) {
                m_phase = (m_frame_phase + 1u) % 4u;
            } else {
                m_phase = 0u;
            }
            m_scene_recorded = false;
        }
    }
}
