module;
#include "pP/UnitTest.h"
#include <cstring>
#include <slang.h>

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // independent post-processing contracts
    // ------------------------------------------------------------------

    namespace PostProcess {
        // ------------------------------------------------------------------
        // presentation values and finite settings
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (presentation_grid_uses_checked_ceil_arithmetic) {
            const auto hd_p1 = makePresentationGrid(int2{1920, 1080}, 1u);
            PPR_TEST_ASSERT(hd_p1.has_value());
            PPR_TEST_ASSERT(hd_p1->m_window.x == 1920 and hd_p1->m_window.y == 1080);
            PPR_TEST_ASSERT(hd_p1->m_resolved.x == 1920 and hd_p1->m_resolved.y == 1080);
            PPR_TEST_ASSERT(hd_p1->m_raster.x == 960 and hd_p1->m_raster.y == 540);
            PPR_TEST_ASSERT(hd_p1->m_backing.x == 1920 and hd_p1->m_backing.y == 1080);

            const auto hd_p2 = makePresentationGrid(int2{1920, 1080}, 2u);
            PPR_TEST_ASSERT(hd_p2.has_value());
            PPR_TEST_ASSERT(hd_p2->m_resolved.x == 960 and hd_p2->m_resolved.y == 540);
            PPR_TEST_ASSERT(hd_p2->m_raster.x == 480 and hd_p2->m_raster.y == 270);
            PPR_TEST_ASSERT(hd_p2->m_backing.x == 960 and hd_p2->m_backing.y == 540);

            const auto hd_p3 = makePresentationGrid(int2{1920, 1080}, 3u);
            PPR_TEST_ASSERT(hd_p3.has_value());
            PPR_TEST_ASSERT(hd_p3->m_resolved.x == 640 and hd_p3->m_resolved.y == 360);
            PPR_TEST_ASSERT(hd_p3->m_raster.x == 320 and hd_p3->m_raster.y == 180);
            PPR_TEST_ASSERT(hd_p3->m_backing.x == 640 and hd_p3->m_backing.y == 360);

            const auto hd_p8 = makePresentationGrid(int2{1920, 1080}, 8u);
            PPR_TEST_ASSERT(hd_p8.has_value());
            PPR_TEST_ASSERT(hd_p8->m_resolved.x == 240 and hd_p8->m_resolved.y == 135);
            PPR_TEST_ASSERT(hd_p8->m_raster.x == 120 and hd_p8->m_raster.y == 68);
            PPR_TEST_ASSERT(hd_p8->m_backing.x == 240 and hd_p8->m_backing.y == 136);

            // Odd framebuffer extents pad the resolve backing and crop at the
            // presentation edge; each axis is independent.
            const auto odd_1919 = makePresentationGrid(int2{1919, 1079}, 2u);
            PPR_TEST_ASSERT(odd_1919.has_value());
            PPR_TEST_ASSERT(odd_1919->m_resolved.x == 960 and odd_1919->m_resolved.y == 540);
            PPR_TEST_ASSERT(odd_1919->m_backing.x == 960 and odd_1919->m_backing.y == 540);

            const auto odd_1921 = makePresentationGrid(int2{1921, 1081}, 2u);
            PPR_TEST_ASSERT(odd_1921.has_value());
            PPR_TEST_ASSERT(odd_1921->m_resolved.x == 961 and odd_1921->m_resolved.y == 541);
            PPR_TEST_ASSERT(odd_1921->m_raster.x == 481 and odd_1921->m_raster.y == 271);
            PPR_TEST_ASSERT(odd_1921->m_backing.x == 962 and odd_1921->m_backing.y == 542);

            const auto single = makePresentationGrid(int2{1, 1}, 1u);
            PPR_TEST_ASSERT(single.has_value());
            PPR_TEST_ASSERT(single->m_resolved.x == 1 and single->m_resolved.y == 1);
            PPR_TEST_ASSERT(single->m_raster.x == 1 and single->m_raster.y == 1);
            PPR_TEST_ASSERT(single->m_backing.x == 2 and single->m_backing.y == 2);

            // Zero framebuffer is caller suspension, never a fabricated extent.
            for (const int2 &rejected: {int2{0, 1080}, int2{1920, 0}, int2{-1, 1080}, int2{1920, -1}}) {
                const auto invalid = makePresentationGrid(rejected, 1u);
                PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
            }

            for (const u32 scale: {0u, 9u, 1000u}) {
                const auto invalid = makePresentationGrid(int2{1920, 1080}, scale);
                PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
            }

            // The widest odd P=1 framebuffer cannot represent its resolve backing.
            const auto overflow = makePresentationGrid(int2{std::numeric_limits<i32>::max(), 1}, 1u);
            PPR_TEST_ASSERT(not overflow and overflow.error() == std::errc::value_too_large);
        };

        PPR_UNIT_TEST (post_settings_reject_nonfinite_and_invalid_tuples) {
            PostProcessSettings valid{};
            PPR_TEST_ASSERT(not validatePostProcessSettings(valid));

            const auto rejects = [](const PostProcessSettings &settings) {
                return validatePostProcessSettings(settings) == std::errc::invalid_argument;
            };

            PostProcessSettings invalid = valid;
            invalid.m_exposure_ev = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(rejects(invalid));
            invalid = valid;
            invalid.m_adaptation_tau = std::numeric_limits<float>::infinity();
            PPR_TEST_ASSERT(rejects(invalid));
            invalid = valid;
            invalid.m_ev_min = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(rejects(invalid));
            invalid = valid;
            invalid.m_bloom_intensity = std::numeric_limits<float>::infinity();
            PPR_TEST_ASSERT(rejects(invalid));
            invalid = valid;
            invalid.m_bloom_threshold = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(rejects(invalid));

            invalid = valid;
            invalid.m_adaptation_tau = 0.0f;
            PPR_TEST_ASSERT(rejects(invalid));
            invalid = valid;
            invalid.m_ev_min = 2.0f;
            invalid.m_ev_max = -2.0f;
            PPR_TEST_ASSERT(rejects(invalid));
            invalid = valid;
            invalid.m_bloom_intensity = -0.01f;
            PPR_TEST_ASSERT(rejects(invalid));
            invalid = valid;
            invalid.m_bloom_threshold = -0.01f;
            PPR_TEST_ASSERT(rejects(invalid));

            // Finite endpoints and the documented game defaults stay accepted.
            PostProcessSettings defaults{};
            defaults.m_auto_exposure = true;
            defaults.m_exposure_ev = 0.0f;
            defaults.m_adaptation_tau = 0.7f;
            defaults.m_ev_min = -4.0f;
            defaults.m_ev_max = 4.0f;
            defaults.m_bloom_enabled = true;
            defaults.m_bloom_intensity = 0.125f;
            defaults.m_bloom_threshold = 1.0f;
            PPR_TEST_ASSERT(not validatePostProcessSettings(defaults));

            defaults.m_exposure_ev = -16.0f;
            PPR_TEST_ASSERT(not validatePostProcessSettings(defaults));
            defaults.m_exposure_ev = 16.0f;
            PPR_TEST_ASSERT(not validatePostProcessSettings(defaults));
            defaults.m_exposure_ev = -16.01f;
            PPR_TEST_ASSERT(rejects(defaults));
            defaults.m_exposure_ev = 16.01f;
            PPR_TEST_ASSERT(rejects(defaults));
        };

        PPR_UNIT_TEST (teardown_is_idempotent_before_initialize) {
            PostProcessPass pass;
            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(not pass.notifyDeviceLost());
            PPR_TEST_ASSERT(not pass.notifyDeviceLost());
        };

        // ------------------------------------------------------------------
        // histogram eye adaptation
        // ------------------------------------------------------------------

        namespace HistogramAdaptation {
            PPR_UNIT_TEST (cpu_histogram_math) {
                constexpr float ev_min = -4.0f;
                constexpr float ev_max = 4.0f;

                // Black frame: every luminance is zero. The log2 clamp keeps the
                // reduction finite; the target EV pins to the upper bound because a
                // dark scene needs positive exposure.
                std::array<u32, PostProcessPass::kHistogramBins> black_hist{};
                black_hist[PostProcessPass::luminanceBin(PostProcessPass::log2Luminance(0.0f))] = 4096u;
                const float black_avg = PostProcessPass::averageLog2Luminance(black_hist, 4096u);
                PPR_TEST_ASSERT(std::isfinite(black_avg));
                PPR_TEST_ASSERT(PostProcessPass::targetEv(black_avg, ev_min, ev_max) == ev_max);

                // Uniform 0.18 middle gray: target EV lands near zero.
                std::array<u32, PostProcessPass::kHistogramBins> uniform_hist{};
                uniform_hist[PostProcessPass::luminanceBin(PostProcessPass::log2Luminance(0.18f))] = 4096u;
                const float uniform_avg = PostProcessPass::averageLog2Luminance(uniform_hist, 4096u);
                const float uniform_ev = PostProcessPass::targetEv(uniform_avg, ev_min, ev_max);
                PPR_TEST_ASSERT(std::isfinite(uniform_ev));
                PPR_TEST_ASSERT(std::abs(uniform_ev) < 0.25f);

                // Bright-emitter dominated: one emitter among a dark background. The
                // percentile clip suppresses the emitter, leaving the dark background
                // to drive a positive target EV, clamped at the upper bound.
                std::array<u32, PostProcessPass::kHistogramBins> emitter_hist{};
                emitter_hist[PostProcessPass::luminanceBin(PostProcessPass::log2Luminance(0.01f))] = 4095u;
                emitter_hist[PostProcessPass::luminanceBin(PostProcessPass::log2Luminance(1000.0f))] = 1u;
                const float emitter_avg = PostProcessPass::averageLog2Luminance(emitter_hist, 4096u);
                const float emitter_ev = PostProcessPass::targetEv(emitter_avg, ev_min, ev_max);
                PPR_TEST_ASSERT(std::isfinite(emitter_ev));
                PPR_TEST_ASSERT(emitter_ev > uniform_ev);
                PPR_TEST_ASSERT(emitter_ev <= ev_max);

                // The target is a radiance multiplier: mapping each source
                // luminance through exp2(targetEv) lands on 18% middle gray.
                constexpr float middle_gray = 0.18f;
                const float dark = 0.018f;
                const float neutral = 0.18f;
                const float bright = 1.8f;
                const float dark_ev = PostProcessPass::targetEv(PostProcessPass::log2Luminance(dark), -10.0f, 10.0f);
                const float neutral_ev = PostProcessPass::targetEv(
                    PostProcessPass::log2Luminance(neutral), -10.0f, 10.0f);
                const float bright_ev = PostProcessPass::targetEv(
                    PostProcessPass::log2Luminance(bright), -10.0f, 10.0f);
                PPR_TEST_ASSERT(dark_ev > 0.0f);
                PPR_TEST_ASSERT(std::abs(neutral_ev) <= 1e-6f);
                PPR_TEST_ASSERT(bright_ev < 0.0f);
                PPR_TEST_ASSERT(std::abs(dark * std::exp2(dark_ev) - middle_gray) <= 1e-4f);
                PPR_TEST_ASSERT(std::abs(neutral * std::exp2(neutral_ev) - middle_gray) <= 1e-4f);
                PPR_TEST_ASSERT(std::abs(bright * std::exp2(bright_ev) - middle_gray) <= 1e-4f);
            };

            PPR_UNIT_TEST (percentile_trim_retains_fractional_mass) {
                // One luminance texel: the 10-90% interior retains exactly 0.8
                // of the mass. Integer bin truncation previously lost it all.
                std::array<u32, PostProcessPass::kHistogramBins> one{};
                const u32 occupied_bin = 37u;
                one[occupied_bin] = 1u;
                const float retained_mass = PostProcessPass::retainedMass(one, 1u);
                PPR_TEST_ASSERT(retained_mass == 0.8f);

                const float one_texel_mean = PostProcessPass::averageLog2Luminance(one, 1u);
                const float occupied_bin_center = PostProcessPass::histogramBinCenterLog2(occupied_bin);
                PPR_TEST_ASSERT(one_texel_mean == occupied_bin_center);

                std::array<u32, PostProcessPass::kHistogramBins> empty{};
                PPR_TEST_ASSERT(PostProcessPass::retainedMass(empty, 0u) == 0.0f);
                PPR_TEST_ASSERT(PostProcessPass::averageLog2Luminance(empty, 0u) == PostProcessPass::kLog2LuminanceMin);
            };

            PPR_UNIT_TEST (cpu_adaptation_alpha) {
                // A 60 Hz frame blends a small fraction, never an instant snap.
                const float slow = PostProcessPass::adaptationAlpha(std::chrono::milliseconds{16}, 0.7f);
                PPR_TEST_ASSERT(slow > 0.0f and slow < 0.1f);

                // A cumulative timestamp (e.g. an hour since startup) would drive the
                // blend to ~1; the adaptation path must never treat request_elapsed as
                // the frame delta, or this is what it would compute.
                const float timestamp = PostProcessPass::adaptationAlpha(std::chrono::hours{1}, 0.7f);
                PPR_TEST_ASSERT(timestamp > 0.99f);

                // Zero delta and non-positive tau never adapt.
                PPR_TEST_ASSERT(PostProcessPass::adaptationAlpha(TimeSpan{}, 0.7f) == 0.0f);
                PPR_TEST_ASSERT(PostProcessPass::adaptationAlpha(std::chrono::milliseconds{16}, 0.0f) == 0.0f);
            };

            // GPU smoke drives the real luminance_histogram.slang + auto_exposure.slang
            // compute entries against a synthetic uniform HDR scene, then reads the
            // adapted offset back once (no per-frame readback in production).
            namespace Gpu {
                struct Fixture {
                    safe_ptr<IShaderService> m_shaders{IShaderService::get()};
                    safe_ptr<IRhiService> m_rhi{IRhiService::get()};
                    rhi::ComPtr<rhi::ICommandQueue> m_queue;
                    FrameFence m_fence;

                    rhi::ComPtr<rhi::ITexture> m_scene;
                    rhi::ComPtr<rhi::ITextureView> m_scene_view;
                    rhi::ComPtr<rhi::ITexture> m_luminance;
                    rhi::ComPtr<rhi::ITextureView> m_luminance_view;
                    rhi::ComPtr<rhi::IBuffer> m_histogram;
                    rhi::ComPtr<rhi::IBuffer> m_target;
                    std::array<rhi::ComPtr<rhi::ITexture>, 2> m_exposure;
                    std::array<rhi::ComPtr<rhi::ITextureView>, 2> m_exposure_view;
                    std::array<rhi::ComPtr<rhi::IShaderProgram>, 4> m_histogram_programs;
                    std::array<rhi::ComPtr<rhi::IComputePipeline>, 4> m_histogram_pipelines;
                    std::array<rhi::ComPtr<rhi::IShaderObject>, 4> m_histogram_roots;
                    rhi::ComPtr<rhi::IShaderProgram> m_auto_program;
                    rhi::ComPtr<rhi::IComputePipeline> m_auto_pipeline;
                    rhi::ComPtr<rhi::IShaderObject> m_auto_root;
                    u32 m_exposure_index{};

                    [[nodiscard]] static rhi::BufferRange range_(rhi::IBuffer *const buffer) noexcept {
                        return {.offset = 0u, .size = buffer->getDesc().size};
                    }

                    void initialize(const float luminance) {
                        PPR_TEST_ASSERT(not m_shaders->initialize());
                        const auto setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
                        const bool validation = setting and *setting == "1";
                        m_rhi->setCoreValidation(true);
                        m_rhi->setGpuAssistedValidation(validation);
#ifdef _WIN32
                        constexpr auto backend = rhi::DeviceType::D3D12;
#else
                        constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
                        PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
                        auto &device = m_rhi->getDevice();
                        PPR_TEST_ASSERT(
                            not make_error_code(device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));
                        PPR_TEST_ASSERT(not m_fence.initialize(device));

                        // Synthetic RGBA32Float scene, uniform luminance, uploaded once.
                        const u32 edge = 64u;
                        std::vector<float> scene(std::size_t(edge) * edge * 4u);
                        for (std::size_t pixel = 0u; pixel < std::size_t(edge) * edge; ++pixel) {
                            scene[pixel * 4u + 0u] = luminance;
                            scene[pixel * 4u + 1u] = luminance;
                            scene[pixel * 4u + 2u] = luminance;
                            scene[pixel * 4u + 3u] = 1.0f;
                        }
                        const rhi::SubresourceData initial{
                            .data = scene.data(),
                            .rowPitch = edge * sizeof(float) * 4u,
                            .slicePitch = std::size_t(edge) * edge * sizeof(float) * 4u};
                        rhi::TextureDesc scene_desc{};
                        scene_desc.type = rhi::TextureType::Texture2D;
                        scene_desc.size = {.width = edge, .height = edge, .depth = 1u};
                        scene_desc.arrayLength = 1u;
                        scene_desc.mipCount = 1u;
                        scene_desc.format = rhi::Format::RGBA32Float;
                        scene_desc.usage = rhi::TextureUsage::ShaderResource;
                        scene_desc.defaultState = rhi::ResourceState::ShaderResource;
                        scene_desc.label = "histogram synthetic scene";
                        PPR_TEST_ASSERT(
                            not make_error_code(device.createTexture(scene_desc, &initial, m_scene.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(m_scene->getDefaultView(m_scene_view.writeRef())));

                        const auto uav_texture = [&](const u32 width, const u32 height, const char *const label,
                                                     rhi::ComPtr<rhi::ITexture> &out,
                                                     rhi::ComPtr<rhi::ITextureView> &view) {
                            rhi::TextureDesc desc{};
                            desc.type = rhi::TextureType::Texture2D;
                            desc.size = {.width = width, .height = height, .depth = 1u};
                            desc.arrayLength = 1u;
                            desc.mipCount = 1u;
                            desc.format = rhi::Format::R32Float;
                            desc.usage = static_cast<rhi::TextureUsage>(
                                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                                static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
                            desc.defaultState = rhi::ResourceState::UnorderedAccess;
                            desc.label = label;
                            PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, out.writeRef())));
                            PPR_TEST_ASSERT(not make_error_code(out->getDefaultView(view.writeRef())));
                        };
                        uav_texture(edge, edge, "histogram luminance", m_luminance, m_luminance_view);

                        // Zero-initialized exposure so the first adaptation frame reads
                        // a finite previous offset (0) rather than uninitialized memory.
                        const float zero = 0.0f;
                        const rhi::SubresourceData zero_initial{
                            .data = &zero, .rowPitch = sizeof(float), .slicePitch = sizeof(float)};
                        const auto exposure_texture = [&](const char *const label, rhi::ComPtr<rhi::ITexture> &out,
                                                          rhi::ComPtr<rhi::ITextureView> &view) {
                            rhi::TextureDesc desc{};
                            desc.type = rhi::TextureType::Texture2D;
                            desc.size = {.width = 1u, .height = 1u, .depth = 1u};
                            desc.arrayLength = 1u;
                            desc.mipCount = 1u;
                            desc.format = rhi::Format::R32Float;
                            desc.usage = static_cast<rhi::TextureUsage>(
                                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                                static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
                            desc.defaultState = rhi::ResourceState::UnorderedAccess;
                            desc.label = label;
                            PPR_TEST_ASSERT(
                                not make_error_code(device.createTexture(desc, &zero_initial, out.writeRef())));
                            PPR_TEST_ASSERT(not make_error_code(out->getDefaultView(view.writeRef())));
                        };
                        exposure_texture("adaptation exposure 0", m_exposure[0], m_exposure_view[0]);
                        exposure_texture("adaptation exposure 1", m_exposure[1], m_exposure_view[1]);

                        const auto device_buffer = [&](const u64 bytes, const char *const label,
                                                       rhi::ComPtr<rhi::IBuffer> &out) {
                            rhi::BufferDesc desc{};
                            desc.size = bytes;
                            desc.elementSize = sizeof(u32);
                            desc.memoryType = rhi::MemoryType::DeviceLocal;
                            desc.usage = static_cast<rhi::BufferUsage>(
                                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                                static_cast<u32>(rhi::BufferUsage::CopySource));
                            desc.defaultState = rhi::ResourceState::UnorderedAccess;
                            desc.label = label;
                            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, nullptr, out.writeRef())));
                        };
                        device_buffer(PostProcessPass::kHistogramBins * sizeof(u32), "histogram bins", m_histogram);
                        device_buffer(sizeof(float), "histogram target EV", m_target);

                        shader::SharedModule histogram_module{};
                        PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                            std::filesystem::current_path() / "shaders" / "luminance_histogram.slang",
                            "luminance_histogram", histogram_module.writeRef()));
                        const char *const histogram_entries[] = {
                            "clearHistogram", "computeLuminance", "buildHistogram", "reduceHistogram"};
                        for (u32 index = 0u; index < 4u; ++index) {
                            shader::ComPtr<slang::IEntryPoint> entry;
                            PPR_TEST_ASSERT(not make_error_code(
                                histogram_module->findEntryPointByName(histogram_entries[index], entry.writeRef())));
                            slang::IComponentType *entries[] = {entry.get()};
                            rhi::ShaderProgramDesc desc{};
                            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                            desc.slangGlobalScope = histogram_module.get();
                            desc.slangEntryPoints = entries;
                            desc.slangEntryPointCount = 1u;
                            shader::Diagnose diagnostics;
                            PPR_TEST_ASSERT(not make_error_code(device.createShaderProgram(
                                desc, m_histogram_programs[index].writeRef(), diagnostics.writeRef())));
                            rhi::ComputePipelineDesc pipeline_desc{};
                            pipeline_desc.program = m_histogram_programs[index];
                            PPR_TEST_ASSERT(not make_error_code(
                                device.createComputePipeline(pipeline_desc, m_histogram_pipelines[index].writeRef())));
                            PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(
                                m_histogram_programs[index].get(), m_histogram_roots[index].writeRef())));
                        }

                        shader::SharedModule auto_module{};
                        PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                            std::filesystem::current_path() / "shaders" / "auto_exposure.slang", "auto_exposure",
                            auto_module.writeRef()));
                        shader::ComPtr<slang::IEntryPoint> auto_entry;
                        PPR_TEST_ASSERT(not make_error_code(
                            auto_module->findEntryPointByName("adaptExposure", auto_entry.writeRef())));
                        slang::IComponentType *auto_entries[] = {auto_entry.get()};
                        rhi::ShaderProgramDesc auto_desc{};
                        auto_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                        auto_desc.slangGlobalScope = auto_module.get();
                        auto_desc.slangEntryPoints = auto_entries;
                        auto_desc.slangEntryPointCount = 1u;
                        shader::Diagnose auto_diagnostics;
                        PPR_TEST_ASSERT(not make_error_code(device.createShaderProgram(
                            auto_desc, m_auto_program.writeRef(), auto_diagnostics.writeRef())));
                        rhi::ComputePipelineDesc auto_pipeline_desc{};
                        auto_pipeline_desc.program = m_auto_program;
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createComputePipeline(auto_pipeline_desc, m_auto_pipeline.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createRootShaderObject(m_auto_program.get(), m_auto_root.writeRef())));
                    }

                    void submit(rhi::ICommandEncoder &encoder) {
                        rhi::ComPtr<rhi::ICommandBuffer> commands;
                        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
                        rhi::ICommandBuffer *pointer = commands.get();
                        rhi::IFence *fence = m_fence.fence();
                        const auto value = m_fence.nextValue();
                        const u64 raw = value;
                        rhi::SubmitDesc description{};
                        description.commandBuffers = &pointer;
                        description.commandBufferCount = 1u;
                        description.signalFences = &fence;
                        description.signalFenceValues = &raw;
                        description.signalFenceCount = 1u;
                        PPR_TEST_ASSERT(not make_error_code(m_queue->submit(description)));
                        m_fence.commitSubmitted(value);
                        PPR_TEST_ASSERT(not m_fence.waitFor(value));
                    }

                    template<typename T>
                    std::vector<T> readBuffer(rhi::IBuffer *const source) {
                        rhi::BufferDesc description{};
                        description.size = source->getDesc().size;
                        description.elementSize = 0u;
                        description.memoryType = rhi::MemoryType::ReadBack;
                        description.usage = rhi::BufferUsage::CopyDestination;
                        description.defaultState = rhi::ResourceState::CopyDestination;
                        description.label = "histogram adaptation readback";
                        rhi::ComPtr<rhi::IBuffer> copy;
                        PPR_TEST_ASSERT(not make_error_code(
                            m_rhi->getDevice().createBuffer(description, nullptr, copy.writeRef())));

                        rhi::ComPtr<rhi::ICommandEncoder> encoder;
                        PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                        encoder->setBufferState(source, rhi::ResourceState::CopySource);
                        encoder->copyBuffer(copy, 0u, source, 0u, source->getDesc().size);
                        encoder->setBufferState(source, rhi::ResourceState::UnorderedAccess);
                        submit(*encoder);

                        std::vector<T> result(source->getDesc().size / sizeof(T));
                        void *mapped{};
                        PPR_TEST_ASSERT(
                            not make_error_code(m_rhi->getDevice().mapBuffer(copy, rhi::CpuAccessMode::Read, &mapped)));
                        std::memcpy(result.data(), mapped, result.size() * sizeof(T));
                        PPR_TEST_ASSERT(not make_error_code(m_rhi->getDevice().unmapBuffer(copy)));
                        return result;
                    }

                    // Runs clear -> luminance downsample -> histogram build -> reduce
                    // and returns the reduced target EV via fenced buffer readback.
                    float reduceTarget() {
                        struct HistogramParameters {
                            u32 m_source_width;
                            u32 m_source_height;
                            u32 m_luminance_width;
                            u32 m_luminance_height;
                            u32 m_reserved0;
                            u32 m_reserved1;
                        };

                        const u32 edge = 64u;
                        const HistogramParameters histogram{edge, edge, edge, edge, 0u, 0u};

                        rhi::ComPtr<rhi::ICommandEncoder> encoder;
                        PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));

                        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                        for (u32 index = 0u; index < 4u; ++index) {
                            rhi::ShaderCursor cursor{m_histogram_roots[index]}, constants{};
                            PPR_TEST_ASSERT(not make_error_code(cursor["g_luminance"].getDereferenced(constants)));
                            PPR_TEST_ASSERT(not make_error_code(constants.setData(&histogram, sizeof(histogram))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_scene"].setBinding(rhi::Binding(m_scene_view))));
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor["g_luminance_image"].setBinding(rhi::Binding(m_luminance_view))));
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor["g_histogram"].setBinding(rhi::Binding(m_histogram, range_(m_histogram)))));
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor["g_target_ev"].setBinding(rhi::Binding(m_target, range_(m_target)))));
                        }

                        encoder->setTextureState(m_luminance, range, rhi::ResourceState::UnorderedAccess);
                        encoder->setBufferState(m_histogram, rhi::ResourceState::UnorderedAccess);
                        encoder->setBufferState(m_target, rhi::ResourceState::UnorderedAccess);

                        const auto dispatch = [&](const u32 index, const u32 x, const u32 y) {
                            auto *const pass = encoder->beginComputePass();
                            PPR_TEST_ASSERT(pass != nullptr);
                            pass->bindPipeline(m_histogram_pipelines[index], m_histogram_roots[index]);
                            pass->dispatchCompute(x, y, 1u);
                            pass->end();
                        };

                        dispatch(0u, 1u, 1u); // clearHistogram
                        encoder->globalBarrier();
                        dispatch(1u, edge / 8u, edge / 8u); // computeLuminance
                        encoder->globalBarrier();
                        dispatch(2u, edge / 8u, edge / 8u); // buildHistogram
                        encoder->globalBarrier();
                        dispatch(3u, 1u, 1u); // reduceHistogram
                        submit(*encoder);

                        const auto out = readBuffer<float>(m_target);
                        PPR_TEST_ASSERT(out.size() == 1u);
                        return out[0];
                    }

                    // Runs one adaptation frame (the histogram target must already be
                    // reduced) and returns the newly written adapted offset.
                    float adapt(const TimeSpan dt, const float tau, const bool enabled) {
                        struct AutoParameters {
                            float m_alpha;
                            float m_ev_min;
                            float m_ev_max;
                            u32 m_enabled;
                        };

                        const AutoParameters automatic{
                            PostProcessPass::adaptationAlpha(dt, tau), -4.0f, 4.0f, enabled ? 1u : 0u};

                        rhi::ComPtr<rhi::ICommandEncoder> encoder;
                        PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));

                        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                        const u32 previous = m_exposure_index;
                        const u32 current = 1u - m_exposure_index;
                        m_exposure_index = current;

                        rhi::ShaderCursor cursor{m_auto_root}, constants{};
                        PPR_TEST_ASSERT(not make_error_code(cursor["g_auto_exposure"].getDereferenced(constants)));
                        PPR_TEST_ASSERT(not make_error_code(constants.setData(&automatic, sizeof(automatic))));
                        PPR_TEST_ASSERT(not make_error_code(
                            cursor["g_target_ev"].setBinding(rhi::Binding(m_target, range_(m_target)))));
                        PPR_TEST_ASSERT(not make_error_code(
                            cursor["g_previous_exposure"].setBinding(rhi::Binding(m_exposure_view[previous]))));
                        PPR_TEST_ASSERT(not make_error_code(
                            cursor["g_exposure"].setBinding(rhi::Binding(m_exposure_view[current]))));

                        encoder->setTextureState(m_exposure[current], range, rhi::ResourceState::UnorderedAccess);
                        auto *const pass = encoder->beginComputePass();
                        PPR_TEST_ASSERT(pass != nullptr);
                        pass->bindPipeline(m_auto_pipeline, m_auto_root);
                        pass->dispatchCompute(1u, 1u, 1u);
                        pass->end();
                        encoder->setTextureState(m_exposure[current], range, rhi::ResourceState::ShaderResource);

                        submit(*encoder);

                        shader::ComPtr<ISlangBlob> blob;
                        rhi::SubresourceLayout layout{};
                        PPR_TEST_ASSERT(not make_error_code(
                            m_rhi->getDevice().readTexture(m_exposure[current], 0u, 0u, blob.writeRef(), &layout)));
                        const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());
                        float value{};
                        std::memcpy(&value, source, sizeof(value));
                        return value;
                    }

                    void shutdown() {
                        if (m_queue and make_error_code(m_queue->waitOnHost()))
                            hal::fatalExit("histogram-adaptation-test-quiescence-unknown");

                        m_auto_root.setNull();
                        m_auto_pipeline.setNull();
                        m_auto_program.setNull();
                        m_histogram_roots = {};
                        m_histogram_pipelines = {};
                        m_histogram_programs = {};
                        m_target.setNull();
                        m_histogram.setNull();
                        m_exposure_view[0].setNull();
                        m_exposure_view[1].setNull();
                        m_exposure[0].setNull();
                        m_exposure[1].setNull();
                        m_luminance_view.setNull();
                        m_luminance.setNull();
                        m_scene_view.setNull();
                        m_scene.setNull();
                        PPR_TEST_ASSERT(not m_fence.shutdown());
                        m_queue.setNull();
                        PPR_TEST_ASSERT(not m_rhi->shutdown());
                        PPR_TEST_ASSERT(not m_shaders->shutdown());
                    }
                };

                PPR_UNIT_TEST (gpu_adaptation_smoke) {
                    constexpr float ev_min = -4.0f;
                    constexpr float ev_max = 4.0f;
                    const TimeSpan dt = std::chrono::milliseconds{16}; // ~60 Hz frame
                    const float tau = 0.7f;
                    const float alpha = PostProcessPass::adaptationAlpha(dt, tau);

                    Fixture fixture;
                    fixture.initialize(0.18f);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    // CPU oracle target for the uniform 0.18 scene (64x64 luminance).
                    // The GPU reduction writes the unclamped target EV; the final clamp
                    // happens in adaptExposure, so compare against the unclamped value.
                    std::array<u32, PostProcessPass::kHistogramBins> histogram{};
                    histogram[PostProcessPass::luminanceBin(PostProcessPass::log2Luminance(0.18f))] = 64u * 64u;
                    const float cpu_target = std::log2(PostProcessPass::kMiddleGrayLuminance) -
                                             PostProcessPass::averageLog2Luminance(histogram, 64u * 64u);

                    // GPU reduced target must match the CPU oracle (numeric, not just
                    // finiteness): a broken histogram/reduction fails here. The
                    // independent .18-scene contract is that the offset is near zero
                    // without consulting the CPU target.
                    const float gpu_target = fixture.reduceTarget();
                    PPR_TEST_ASSERT(std::isfinite(gpu_target));
                    PPR_TEST_ASSERT(std::abs(gpu_target) < 0.25f);
                    PPR_TEST_ASSERT(std::abs(gpu_target - cpu_target) < 0.05f);

                    // A 60 Hz frame adapts slowly toward the target: the first frame is
                    // target*alpha, not an instant snap to the target.
                    const float first = fixture.adapt(dt, tau, true);
                    PPR_TEST_ASSERT(std::isfinite(first));
                    PPR_TEST_ASSERT(std::abs(first - cpu_target * alpha) < 0.05f);
                    PPR_TEST_ASSERT(std::abs(first) < std::abs(cpu_target) * 0.5f);

                    // A second identical frame moves a little further, still bounded.
                    const float second = fixture.adapt(dt, tau, true);
                    PPR_TEST_ASSERT(std::isfinite(second) and second >= ev_min and second <= ev_max);
                    PPR_TEST_ASSERT(std::abs(second - first) < 0.05f);

                    // Fixed exposure (disabled) writes the zero offset deterministically.
                    const float fixed_a = fixture.adapt(dt, tau, false);
                    const float fixed_b = fixture.adapt(dt, tau, false);
                    PPR_TEST_ASSERT(fixed_a == 0.0f);
                    PPR_TEST_ASSERT(fixed_b == 0.0f);
                };

                // Independent GPU contract in actual scene-radiance units: a dark
                // uniform scene must produce a positive target EV and therefore a
                // brighter output after the display-side exp2 multiply. This does not
                // consult the CPU target formula.
                PPR_UNIT_TEST (gpu_dark_scene_target_ev_raises_exposure) {
                    constexpr float dark_luminance = 0.018f;

                    Fixture fixture;
                    fixture.initialize(dark_luminance);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    const float gpu_target = fixture.reduceTarget();
                    PPR_TEST_ASSERT(std::isfinite(gpu_target));
                    PPR_TEST_ASSERT(gpu_target > 0.0f);
                    PPR_TEST_ASSERT(dark_luminance * std::exp2(gpu_target) > dark_luminance);
                };
            }

            const UnitTest histogram_adaptation = UnitTest::Named("histogram_adaptation") /
                                                  [](UnitTest::IRun &run) -> void {
                run.recurse(
                    {cpu_histogram_math, percentile_trim_retains_fractional_mass, cpu_adaptation_alpha,
                        Gpu::gpu_adaptation_smoke, Gpu::gpu_dark_scene_target_ev_raises_exposure});
            };
        }

        // ------------------------------------------------------------------
        // ceil-sized bloom
        // ------------------------------------------------------------------

        namespace SpdBloom {
            PPR_UNIT_TEST (cpu_bloom_ceil_chain_layout) {
                // A 1080p picture: ceil(R/2) level 0 and ceil-halved levels,
                // stopping after six levels because 30x17 is not yet 1x1.
                const std::array<int2, PostProcessPass::kMaxBloomLevels> bloom_extents =
                    PostProcessPass::bloomLevelExtents(int2{1920, 1080});
                const std::array<int2, PostProcessPass::kMaxBloomLevels> ceil_chain_to_one_or_six{
                    int2{960, 540}, int2{480, 270}, int2{240, 135}, int2{120, 68}, int2{60, 34}, int2{30, 17}};
                PPR_TEST_ASSERT(PostProcessPass::bloomLevelCount(int2{1920, 1080}) == 6u);
                for (u32 level = 0u; level < PostProcessPass::kMaxBloomLevels; ++level) {
                    PPR_TEST_ASSERT(bloom_extents[level].x == ceil_chain_to_one_or_six[level].x);
                    PPR_TEST_ASSERT(bloom_extents[level].y == ceil_chain_to_one_or_six[level].y);
                }

                // Odd extents use ceil, not floor: the last partial row/column is
                // content and is replicated by the 2x2 source filter.
                const std::array<int2, PostProcessPass::kMaxBloomLevels> odd_extents =
                    PostProcessPass::bloomLevelExtents(int2{101, 51});
                const std::array<int2, PostProcessPass::kMaxBloomLevels> odd_ceil_chain{
                    int2{51, 26}, int2{26, 13}, int2{13, 7}, int2{7, 4}, int2{4, 2}, int2{2, 1}};
                PPR_TEST_ASSERT(PostProcessPass::bloomLevelCount(int2{101, 51}) == 6u);
                for (u32 level = 0u; level < PostProcessPass::kMaxBloomLevels; ++level) {
                    PPR_TEST_ASSERT(odd_extents[level].x == odd_ceil_chain[level].x);
                    PPR_TEST_ASSERT(odd_extents[level].y == odd_ceil_chain[level].y);
                }

                // A tiny picture stops at its 1x1 level (still a valid cohort).
                const auto one = PostProcessPass::bloomLevelExtents(int2{1, 1});
                PPR_TEST_ASSERT(PostProcessPass::bloomLevelCount(int2{1, 1}) == 1u);
                PPR_TEST_ASSERT(one[0].x == 1 and one[0].y == 1);
                for (const int2 &extent: one) {
                    PPR_TEST_ASSERT(extent.x == 1 and extent.y == 1);
                }

                const auto two = PostProcessPass::bloomLevelExtents(int2{2, 2});
                PPR_TEST_ASSERT(PostProcessPass::bloomLevelCount(int2{2, 2}) == 1u);
                PPR_TEST_ASSERT(two[0].x == 1 and two[0].y == 1);
            };

            namespace Gpu {
                struct BloomParams {
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
                static_assert(sizeof(BloomParams) == 64u);

                // IEEE-754 half->float. The bloom readback decodes the same half
                // bytes the GPU wrote.
                [[nodiscard]] float halfToFloat(const u16 value) noexcept {
                    const u32 sign = (value >> 15) & 1u;
                    const u32 exponent = (value >> 10) & 0x1fu;
                    const u32 mantissa = value & 0x3ffu;

                    u32 bits;
                    if (exponent == 0u) {
                        if (mantissa == 0u) {
                            bits = sign << 31;
                        } else {
                            u32 e = 127u - 15u + 1u;
                            u32 m = mantissa;
                            while ((m & 0x400u) == 0u) {
                                m <<= 1;
                                --e;
                            }
                            m &= 0x3ffu;
                            bits = (sign << 31) | (e << 23) | (m << 13);
                        }
                    } else if (exponent == 0x1fu) {
                        bits = (sign << 31) | (0xffu << 23) | (mantissa << 13);
                    } else {
                        bits = (sign << 31) | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
                    }
                    return std::bit_cast<float>(bits);
                }

                struct Fixture {
                    safe_ptr<IShaderService> m_shaders{IShaderService::get()};
                    safe_ptr<IRhiService> m_rhi{IRhiService::get()};
                    rhi::ComPtr<rhi::ICommandQueue> m_queue;
                    FrameFence m_fence;

                    std::array<rhi::ComPtr<rhi::ITexture>, PostProcessPass::kMaxBloomLevels> m_bloom{};
                    std::array<rhi::ComPtr<rhi::ITextureView>, PostProcessPass::kMaxBloomLevels> m_bloom_views{};
                    std::array<rhi::ComPtr<rhi::IShaderProgram>, 3> m_programs{};
                    std::array<rhi::ComPtr<rhi::IComputePipeline>, 3> m_pipelines{};
                    std::array<rhi::ComPtr<rhi::IShaderObject>, 3> m_roots{};
                    rhi::ComPtr<rhi::ISampler> m_sampler;
                    rhi::DescriptorHandle m_sampler_handle{};
                    u32 m_levels{};

                    void initialize(const u32 edge) {
                        PPR_TEST_ASSERT(not m_shaders->initialize());
                        const auto setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
                        const bool validation = setting and *setting == "1";
                        m_rhi->setCoreValidation(true);
                        m_rhi->setGpuAssistedValidation(validation);
#ifdef _WIN32
                        constexpr auto backend = rhi::DeviceType::D3D12;
#else
                        constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
                        PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
                        auto &device = m_rhi->getDevice();
                        PPR_TEST_ASSERT(
                            not make_error_code(device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));
                        PPR_TEST_ASSERT(not m_fence.initialize(device));

                        m_levels = PostProcessPass::bloomLevelCount(int2{int(edge), int(edge)});
                        for (u32 level = 0u; level < m_levels; ++level) {
                            const int2 extent = PostProcessPass::bloomLevelExtent(int2{int(edge), int(edge)}, level);
                            rhi::TextureDesc desc{};
                            desc.type = rhi::TextureType::Texture2D;
                            desc.size = {
                                .width = static_cast<u32>(extent.x), .height = static_cast<u32>(extent.y), .depth = 1u};
                            desc.arrayLength = 1u;
                            desc.mipCount = 1u;
                            desc.format = rhi::Format::RGBA16Float;
                            desc.usage = static_cast<rhi::TextureUsage>(
                                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                                static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
                            desc.defaultState = rhi::ResourceState::UnorderedAccess;
                            desc.label = "bloom level";
                            PPR_TEST_ASSERT(
                                not make_error_code(device.createTexture(desc, nullptr, m_bloom[level].writeRef())));
                            PPR_TEST_ASSERT(
                                not make_error_code(m_bloom[level]->getDefaultView(m_bloom_views[level].writeRef())));
                        }

                        rhi::SamplerDesc sampler{};
                        sampler.minFilter = rhi::TextureFilteringMode::Linear;
                        sampler.magFilter = rhi::TextureFilteringMode::Linear;
                        sampler.mipFilter = rhi::TextureFilteringMode::Linear;
                        sampler.addressU = rhi::TextureAddressingMode::ClampToEdge;
                        sampler.addressV = rhi::TextureAddressingMode::ClampToEdge;
                        sampler.addressW = rhi::TextureAddressingMode::ClampToEdge;
                        sampler.maxAnisotropy = 1u;
                        PPR_TEST_ASSERT(not make_error_code(device.createSampler(sampler, m_sampler.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(m_sampler->getDescriptorHandle(&m_sampler_handle)));

                        shader::SharedModule module{};
                        PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                            std::filesystem::current_path() / "shaders" / "spd_bloom.slang", "spd_bloom",
                            module.writeRef()));
                        const char *const entries[] = {"prefilterBloom", "downsampleBloom", "upsampleBloom"};
                        for (u32 index = 0u; index < 3u; ++index) {
                            shader::ComPtr<slang::IEntryPoint> entry;
                            PPR_TEST_ASSERT(
                                not make_error_code(module->findEntryPointByName(entries[index], entry.writeRef())));
                            slang::IComponentType *component_entries[] = {entry.get()};
                            rhi::ShaderProgramDesc desc{};
                            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                            desc.slangGlobalScope = module.get();
                            desc.slangEntryPoints = component_entries;
                            desc.slangEntryPointCount = 1u;
                            shader::Diagnose diagnostics;
                            PPR_TEST_ASSERT(not make_error_code(device.createShaderProgram(
                                desc, m_programs[index].writeRef(), diagnostics.writeRef())));
                            rhi::ComputePipelineDesc pipeline_desc{};
                            pipeline_desc.program = m_programs[index];
                            PPR_TEST_ASSERT(not make_error_code(
                                device.createComputePipeline(pipeline_desc, m_pipelines[index].writeRef())));
                            PPR_TEST_ASSERT(not make_error_code(
                                device.createRootShaderObject(m_programs[index].get(), m_roots[index].writeRef())));
                        }
                    }

                    void submit(rhi::ICommandEncoder &encoder) {
                        rhi::ComPtr<rhi::ICommandBuffer> commands;
                        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
                        rhi::ICommandBuffer *pointer = commands.get();
                        rhi::IFence *fence = m_fence.fence();
                        const auto value = m_fence.nextValue();
                        const u64 raw = value;
                        rhi::SubmitDesc description{};
                        description.commandBuffers = &pointer;
                        description.commandBufferCount = 1u;
                        description.signalFences = &fence;
                        description.signalFenceValues = &raw;
                        description.signalFenceCount = 1u;
                        PPR_TEST_ASSERT(not make_error_code(m_queue->submit(description)));
                        m_fence.commitSubmitted(value);
                        PPR_TEST_ASSERT(not m_fence.waitFor(value));
                    }

                    // Uploads a synthetic scene (uniform background plus one bright
                    // block), runs the full prefilter/downsample/upsample chain, and
                    // returns the accumulated level 0 decoded to linear float RGB.
                    // adapted_ev is the 1x1 exposure offset the prefilter samples
                    // (0 = fixed exposure).
                    std::vector<std::array<float, 3>> run(
                        const u32 edge, const float background, const float block_value,
                        const float adapted_ev = 0.0f) {
                        const int2 resolved{int(edge), int(edge)};
                        const int2 level0 = PostProcessPass::bloomLevelExtent(resolved, 0u);
                        const u32 block_edge = edge / 16u;
                        const u32 block_origin = (edge - block_edge) / 2u;

                        std::vector<float> scene(std::size_t(edge) * edge * 4u);
                        for (u32 y = 0u; y < edge; ++y) {
                            for (u32 x = 0u; x < edge; ++x) {
                                const bool in_block = x >= block_origin and
                                                      x < block_origin + block_edge and
                                                      y >= block_origin and
                                                      y < block_origin + block_edge;
                                const float value = in_block ? block_value : background;
                                scene[(std::size_t(y) * edge + x) * 4u + 0u] = value;
                                scene[(std::size_t(y) * edge + x) * 4u + 1u] = value;
                                scene[(std::size_t(y) * edge + x) * 4u + 2u] = value;
                                scene[(std::size_t(y) * edge + x) * 4u + 3u] = 1.0f;
                            }
                        }
                        const rhi::SubresourceData initial{
                            .data = scene.data(),
                            .rowPitch = edge * sizeof(float) * 4u,
                            .slicePitch = std::size_t(edge) * edge * sizeof(float) * 4u};
                        rhi::TextureDesc scene_desc{};
                        scene_desc.type = rhi::TextureType::Texture2D;
                        scene_desc.size = {.width = edge, .height = edge, .depth = 1u};
                        scene_desc.arrayLength = 1u;
                        scene_desc.mipCount = 1u;
                        scene_desc.format = rhi::Format::RGBA32Float;
                        scene_desc.usage = rhi::TextureUsage::ShaderResource;
                        scene_desc.defaultState = rhi::ResourceState::ShaderResource;
                        scene_desc.label = "bloom synthetic scene";
                        rhi::ComPtr<rhi::ITexture> scene_tex;
                        rhi::ComPtr<rhi::ITextureView> scene_view;
                        auto &device = m_rhi->getDevice();
                        PPR_TEST_ASSERT(
                            not make_error_code(device.createTexture(scene_desc, &initial, scene_tex.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(scene_tex->getDefaultView(scene_view.writeRef())));

                        // 1x1 adapted-exposure offset sampled by the prefilter.
                        const float adapted_value = adapted_ev;
                        const rhi::SubresourceData exposure_initial{
                            .data = &adapted_value, .rowPitch = sizeof(float), .slicePitch = sizeof(float)};
                        rhi::TextureDesc exposure_desc{};
                        exposure_desc.type = rhi::TextureType::Texture2D;
                        exposure_desc.size = {.width = 1u, .height = 1u, .depth = 1u};
                        exposure_desc.arrayLength = 1u;
                        exposure_desc.mipCount = 1u;
                        exposure_desc.format = rhi::Format::R32Float;
                        exposure_desc.usage = rhi::TextureUsage::ShaderResource;
                        exposure_desc.defaultState = rhi::ResourceState::ShaderResource;
                        exposure_desc.label = "bloom exposure offset";
                        rhi::ComPtr<rhi::ITexture> exposure_tex;
                        rhi::ComPtr<rhi::ITextureView> exposure_view;
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createTexture(exposure_desc, &exposure_initial, exposure_tex.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(exposure_tex->getDefaultView(exposure_view.writeRef())));

                        BloomParams params{};
                        params.m_source_width = edge;
                        params.m_source_height = edge;
                        params.m_threshold = 1.0f;
                        params.m_soft_knee = PostProcessPass::kBloomSoftKnee;
                        params.m_manual_ev = 0.0f;

                        rhi::ComPtr<rhi::ICommandEncoder> encoder;
                        PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));

                        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                        const auto dispatch = [&](const u32 program, const u32 x, const u32 y) {
                            auto *const pass = encoder->beginComputePass();
                            PPR_TEST_ASSERT(pass != nullptr);
                            pass->bindPipeline(m_pipelines[program], m_roots[program]);
                            pass->dispatchCompute(x, y, 1u);
                            pass->end();
                        };

                        {
                            params.m_dst_width = static_cast<u32>(level0.x);
                            params.m_dst_height = static_cast<u32>(level0.y);
                            params.m_level = 0u;
                            rhi::ShaderCursor cursor{m_roots[0]}, constants{};
                            PPR_TEST_ASSERT(not make_error_code(cursor["g_bloom"].getDereferenced(constants)));
                            PPR_TEST_ASSERT(not make_error_code(constants.setData(&params, sizeof(params))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_scene"].setBinding(rhi::Binding(scene_view))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_exposure"].setBinding(rhi::Binding(exposure_view))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_down_dst"].setBinding(rhi::Binding(m_bloom_views[0]))));
                        }

                        encoder->setTextureState(m_bloom[0], range, rhi::ResourceState::UnorderedAccess);
                        dispatch(0u, (static_cast<u32>(level0.x) + 7u) / 8u, (static_cast<u32>(level0.y) + 7u) / 8u);
                        encoder->globalBarrier();

                        for (u32 level = 1u; level < m_levels; ++level) {
                            const int2 src = PostProcessPass::bloomLevelExtent(resolved, level - 1u);
                            const int2 dst = PostProcessPass::bloomLevelExtent(resolved, level);
                            params.m_src_width = static_cast<u32>(src.x);
                            params.m_src_height = static_cast<u32>(src.y);
                            params.m_dst_width = static_cast<u32>(dst.x);
                            params.m_dst_height = static_cast<u32>(dst.y);
                            params.m_level = level;
                            rhi::ShaderCursor cursor{m_roots[1]}, constants{};
                            PPR_TEST_ASSERT(not make_error_code(cursor["g_bloom"].getDereferenced(constants)));
                            PPR_TEST_ASSERT(not make_error_code(constants.setData(&params, sizeof(params))));
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor["g_down_src"].setBinding(rhi::Binding(m_bloom_views[level - 1u]))));
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor["g_down_dst"].setBinding(rhi::Binding(m_bloom_views[level]))));
                            encoder->setTextureState(m_bloom[level - 1u], range, rhi::ResourceState::ShaderResource);
                            encoder->setTextureState(m_bloom[level], range, rhi::ResourceState::UnorderedAccess);
                            dispatch(1u, (static_cast<u32>(dst.x) + 7u) / 8u, (static_cast<u32>(dst.y) + 7u) / 8u);
                            encoder->globalBarrier();
                        }

                        for (u32 level = m_levels; level-- > 0u;) {
                            const bool add_upsample = level + 1u < m_levels;
                            const u32 source_level = add_upsample ? level + 1u : level;
                            const int2 src = PostProcessPass::bloomLevelExtent(resolved, source_level);
                            const int2 dst = PostProcessPass::bloomLevelExtent(resolved, level);
                            params.m_src_width = static_cast<u32>(src.x);
                            params.m_src_height = static_cast<u32>(src.y);
                            params.m_dst_width = static_cast<u32>(dst.x);
                            params.m_dst_height = static_cast<u32>(dst.y);
                            params.m_level = level;
                            params.m_add_upsample = add_upsample ? 1u : 0u;
                            rhi::ShaderCursor cursor{m_roots[2]}, constants{};
                            PPR_TEST_ASSERT(not make_error_code(cursor["g_bloom"].getDereferenced(constants)));
                            PPR_TEST_ASSERT(not make_error_code(constants.setData(&params, sizeof(params))));
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor["g_up_src"].setBinding(rhi::Binding(m_bloom_views[source_level]))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_up_dst"].setBinding(rhi::Binding(m_bloom_views[level]))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_linear"].setDescriptorHandle(m_sampler_handle)));
                            if (add_upsample) {
                                encoder->setTextureState(
                                    m_bloom[source_level], range, rhi::ResourceState::ShaderResource);
                            }
                            encoder->setTextureState(m_bloom[level], range, rhi::ResourceState::UnorderedAccess);
                            dispatch(2u, (static_cast<u32>(dst.x) + 7u) / 8u, (static_cast<u32>(dst.y) + 7u) / 8u);
                            encoder->globalBarrier();
                        }

                        encoder->setTextureState(m_bloom[0], range, rhi::ResourceState::ShaderResource);
                        submit(*encoder);

                        shader::ComPtr<ISlangBlob> blob;
                        rhi::SubresourceLayout layout{};
                        PPR_TEST_ASSERT(not make_error_code(
                            device.readTexture(m_bloom[0].get(), 0u, 0u, blob.writeRef(), &layout)));
                        const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer());

                        std::vector<std::array<float, 3>> result(std::size_t(level0.x) * std::size_t(level0.y));
                        for (u32 y = 0u; y < u32(level0.y); ++y) {
                            for (u32 x = 0u; x < u32(level0.x); ++x) {
                                const std::byte *const pixel = bytes + y * layout.rowPitch + x * 8u;
                                std::array<u16, 4> halves{};
                                std::memcpy(halves.data(), pixel, sizeof(halves));
                                result[std::size_t(y) * std::size_t(level0.x) + x] = {
                                    halfToFloat(halves[0]), halfToFloat(halves[1]), halfToFloat(halves[2])};
                            }
                        }

                        scene_view.setNull();
                        scene_tex.setNull();
                        exposure_view.setNull();
                        exposure_tex.setNull();
                        encoder.setNull();
                        return result;
                    }

                    void shutdown() {
                        if (m_queue and make_error_code(m_queue->waitOnHost()))
                            hal::fatalExit("bloom-test-quiescence-unknown");

                        m_roots = {};
                        m_pipelines = {};
                        m_programs = {};
                        m_bloom_views = {};
                        m_bloom = {};
                        m_sampler.setNull();
                        m_sampler_handle = {};
                        PPR_TEST_ASSERT(not m_fence.shutdown());
                        m_queue.setNull();
                        PPR_TEST_ASSERT(not m_rhi->shutdown());
                        PPR_TEST_ASSERT(not m_shaders->shutdown());
                    }
                };

                [[nodiscard]] float luminanceOf(const std::array<float, 3> &pixel) noexcept {
                    return 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
                }

                PPR_UNIT_TEST (gpu_bloom_smoke) {
                    constexpr u32 edge = 128u;

                    Fixture fixture;
                    fixture.initialize(edge);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    const int2 level0 = PostProcessPass::bloomLevelExtent(int2{int(edge), int(edge)}, 0u);
                    const auto at = [&](const std::vector<std::array<float, 3>> &result, const u32 x, const u32 y) {
                        return luminanceOf(result[std::size_t(y) * u32(level0.x) + x]);
                    };

                    const auto bright = fixture.run(edge, 0.05f, 20.0f);
                    PPR_TEST_ASSERT(bright.size() == std::size_t(level0.x) * std::size_t(level0.y));

                    // Finite everywhere, and the bright block leaves a non-zero trail.
                    bool all_finite = true;
                    for (const auto &pixel: bright) {
                        all_finite = all_finite and
                                     std::isfinite(pixel[0]) and
                                     std::isfinite(pixel[1]) and
                                     std::isfinite(pixel[2]);
                    }
                    PPR_TEST_ASSERT(all_finite);

                    const u32 block_centre = edge / 4u;
                    PPR_TEST_ASSERT(at(bright, block_centre, block_centre) > 0.01f);
                    PPR_TEST_ASSERT(at(bright, block_centre + 2u, block_centre) > 1e-3f);
                    PPR_TEST_ASSERT(at(bright, block_centre + 8u, block_centre) > 1e-4f);
                    PPR_TEST_ASSERT(
                        at(bright, block_centre, block_centre) > at(bright, block_centre + 8u, block_centre));

                    // A black picture stays black: the soft-knee bright pass emits zero.
                    const auto black = fixture.run(edge, 0.0f, 0.0f);
                    for (const auto &pixel: black) {
                        PPR_TEST_ASSERT(std::abs(pixel[0]) < 1e-6f);
                        PPR_TEST_ASSERT(std::abs(pixel[1]) < 1e-6f);
                        PPR_TEST_ASSERT(std::abs(pixel[2]) < 1e-6f);
                    }
                };

                PPR_UNIT_TEST (gpu_bloom_exposure_aware) {
                    constexpr u32 edge = 128u;

                    Fixture fixture;
                    fixture.initialize(edge);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    // A uniform 0.5 picture is below the exposed threshold (1.0), so a
                    // fixed exposure emits no bloom...
                    const auto fixed = fixture.run(edge, 0.5f, 0.5f, 0.0f);
                    for (const auto &pixel: fixed) {
                        PPR_TEST_ASSERT(std::abs(pixel[0]) < 1e-6f);
                        PPR_TEST_ASSERT(std::abs(pixel[1]) < 1e-6f);
                        PPR_TEST_ASSERT(std::abs(pixel[2]) < 1e-6f);
                    }

                    // ...but a +2 EV adapted offset divides the scene-unit threshold to
                    // 0.25, so the identical picture now blooms.
                    const auto exposed = fixture.run(edge, 0.5f, 0.5f, 2.0f);
                    float peak = 0.0f;
                    for (const auto &pixel: exposed) {
                        peak = std::max(peak, luminanceOf(pixel));
                    }
                    PPR_TEST_ASSERT(peak > 1e-3f);
                };

                PPR_UNIT_TEST (gpu_bloom_bounded_uniform) {
                    constexpr u32 edge = 128u;

                    Fixture fixture;
                    fixture.initialize(edge);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    // A uniform bright picture accumulates exactly
                    // sum(w_l) * prefilter(20) when the raw levels stay unweighted
                    // and each weight is applied once during the descent. A
                    // compounded chain lands near 1.56 instead of ~1.94 and fails
                    // both the numeric and lower-bound assertions below.
                    constexpr float uniform_value = 20.0f;
                    constexpr float threshold = 1.0f;
                    constexpr float soft_knee = 0.5f;
                    float brightness = std::max(uniform_value - threshold, 0.0f);
                    brightness = brightness < 2.0f * soft_knee ? brightness * brightness / (4.0f * soft_knee + 1e-4f)
                                                               : brightness - soft_knee;
                    const float prefilter = brightness / std::max(uniform_value, 1e-4f) * uniform_value;
                    const float raw = prefilter / (1.0f + prefilter);
                    float specified = 0.0f;
                    for (const float weight: PostProcessPass::kBloomLevelWeights) {
                        specified += weight * raw;
                    }

                    const auto bloomed = fixture.run(edge, uniform_value, uniform_value, 0.0f);
                    float peak = 0.0f;
                    for (const auto &pixel: bloomed) {
                        peak = std::max(peak, luminanceOf(pixel));
                        PPR_TEST_ASSERT(
                            std::isfinite(pixel[0]) and std::isfinite(pixel[1]) and std::isfinite(pixel[2]));
                    }
                    PPR_TEST_ASSERT(std::abs(peak - specified) < 0.02f);
                    PPR_TEST_ASSERT(peak > 1.8f);
                    PPR_TEST_ASSERT(peak < 4.0f);
                };

                PPR_UNIT_TEST (gpu_bloom_single_level_not_halved) {
                    constexpr u32 edge = 2u;

                    Fixture fixture;
                    fixture.initialize(edge);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    const int2 level0 = PostProcessPass::bloomLevelExtent(int2{int(edge), int(edge)}, 0u);
                    PPR_TEST_ASSERT(level0.x == 1 and level0.y == 1);
                    PPR_TEST_ASSERT(PostProcessPass::bloomLevelCount(int2{int(edge), int(edge)}) == 1u);

                    // A uniform bright picture through a single-level chain keeps the
                    // prefilter intact: no coarse level exists to alias onto level 0.
                    const auto bloomed = fixture.run(edge, 20.0f, 20.0f, 0.0f);
                    PPR_TEST_ASSERT(bloomed.size() == 1u);
                    const float peak = luminanceOf(bloomed[0]);
                    PPR_TEST_ASSERT(std::isfinite(peak));
                    PPR_TEST_ASSERT(peak > 0.7f);
                };
            }

            const UnitTest spd_bloom = UnitTest::Named("spd_bloom") / [](UnitTest::IRun &run) -> void {
                run.recurse(
                    {cpu_bloom_ceil_chain_layout, Gpu::gpu_bloom_smoke, Gpu::gpu_bloom_exposure_aware,
                        Gpu::gpu_bloom_bounded_uniform, Gpu::gpu_bloom_single_level_not_halved});
            };
        }

        // ------------------------------------------------------------------
        // pass-owned output lifetime and settlement
        // ------------------------------------------------------------------

        namespace OutputLifetime {
            PPR_UNIT_TEST (gpu_prepare_rejects_mismatched_view_and_output_is_reusable) {
                const auto rhi = IRhiService::get();
                const auto shaders = IShaderService::get();
                PPR_TEST_ASSERT(rhi.isValid() and shaders.isValid());
                PPR_TEST_ASSERT(not shaders->initialize());
                const auto setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
                const bool validation = setting and *setting == "1";
                rhi->setCoreValidation(true);
                rhi->setGpuAssistedValidation(validation);
#ifdef _WIN32
                constexpr auto backend = rhi::DeviceType::D3D12;
#else
                constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
                PPR_TEST_ASSERT(not rhi->initialize(backend, *shaders));
                PPR_DEFER {
                    PPR_TEST_ASSERT(not rhi->shutdown());
                    PPR_TEST_ASSERT(not shaders->shutdown());
                };

                auto &device = rhi->getDevice();
                PostProcessPass pass;
                PPR_TEST_ASSERT(not pass.initialize(*shaders, std::filesystem::current_path()));
                PPR_DEFER {
                    PPR_TEST_ASSERT(not pass.shutdown());
                };

                const GridWorldIdentity world{.m_job_id = 1u, .m_world_generation = 1u, .m_content_fingerprint = 1u};
                const int2 resolved{17, 9};
                PPR_TEST_ASSERT(not pass.beginFrame(device, resolved, world, kFenceValueNone));
                PPR_TEST_ASSERT(pass.outputTexture().getDesc().format == rhi::Format::RGBA16Float);
                PPR_TEST_ASSERT(pass.outputTexture().getDesc().size.width == 17u);
                PPR_TEST_ASSERT(pass.outputTexture().getDesc().size.height == 9u);

                // A mismatched valid rectangle is rejected before any submission;
                // discarding the attempt leaves the output usable.
                const rhi::ComPtr<rhi::ITextureView> rejected_view = pass.outputTexture().getDefaultView();
                const rhi::ComPtr<rhi::ICommandEncoder> encoder = [&] {
                    rhi::ComPtr<rhi::ICommandQueue> queue;
                    PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, queue.writeRef())));
                    rhi::ComPtr<rhi::ICommandEncoder> created;
                    PPR_TEST_ASSERT(not make_error_code(queue->createCommandEncoder(created.writeRef())));
                    return created;
                }();

                const PreRenderContext context{device, *encoder, kFenceValueNone, {}};
                const ResolvedHdrView mismatched{
                    .m_view = rejected_view,
                    .m_world = world,
                    .m_backing = int2{17, 9},
                    .m_valid = int2{16, 9},
                };
                PPR_TEST_ASSERT(
                    pass.prepare(context, mismatched, PostProcessSettings{}, TimeSpan{}) ==
                    std::errc::invalid_argument);
                pass.discardUnsubmitted();

                PPR_TEST_ASSERT(not pass.beginFrame(device, resolved, world, kFenceValueNone));
                PPR_TEST_ASSERT(pass.outputTexture().getDesc().size.width == 17u);

                // Completion collection releases the retained binding exactly once.
                PPR_TEST_ASSERT(not pass.collectCompleted(0u));
            };
        }

        const UnitTest output_lifetime = UnitTest::Named("output_lifetime") / [](UnitTest::IRun &run) -> void {
            run.recurse({OutputLifetime::gpu_prepare_rejects_mismatched_view_and_output_is_reusable});
        };

        // ------------------------------------------------------------------
        // padded backing, R=1 and odd non-square GPU coverage
        // ------------------------------------------------------------------

        namespace Backing {
            [[nodiscard]] u16 toHalf(const float value) noexcept {
                const u32 bits = std::bit_cast<u32>(value);
                const u32 sign = (bits >> 31u) & 1u;
                if ((bits & 0x7fffffffu) == 0u) {
                    return static_cast<u16>(sign << 15u);
                }

                const u32 exponent = ((bits >> 23u) & 0xffu) - 127u + 15u;
                const u32 mantissa = (bits >> 13u) & 0x3ffu;
                return static_cast<u16>((sign << 15u) | (exponent << 10u) | mantissa);
            }

            [[nodiscard]] float fromHalf(const u16 value) noexcept {
                const u32 sign = (value >> 15u) & 1u;
                const u32 exponent = (value >> 10u) & 0x1fu;
                const u32 mantissa = value & 0x3ffu;
                u32 bits{};
                if (exponent == 0u) {
                    bits = sign << 31u;
                } else {
                    bits = (sign << 31u) | ((exponent - 15u + 127u) << 23u) | (mantissa << 13u);
                }

                return std::bit_cast<float>(bits);
            }

            struct Capture {
                safe_ptr<IRhiService> m_rhi{IRhiService::get()};
                safe_ptr<IShaderService> m_shaders{IShaderService::get()};
                rhi::ComPtr<rhi::ICommandQueue> m_queue;
                FrameFence m_fence;
                PostProcessPass m_pass;
                FenceCompletionValue m_value{kFenceValueNone};

                void initialize() {
                    PPR_TEST_ASSERT(not m_shaders->initialize());
                    const auto setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
                    const bool validation = setting and *setting == "1";
                    m_rhi->setCoreValidation(true);
                    m_rhi->setGpuAssistedValidation(validation);
#ifdef _WIN32
                    constexpr auto backend = rhi::DeviceType::D3D12;
#else
                    constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
                    PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
                    auto &device = m_rhi->getDevice();
                    PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));
                    PPR_TEST_ASSERT(not m_fence.initialize(device));
                    PPR_TEST_ASSERT(not m_pass.initialize(*m_shaders, std::filesystem::current_path()));
                }

                void submit(rhi::ICommandEncoder &encoder) {
                    rhi::ComPtr<rhi::ICommandBuffer> commands;
                    PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
                    rhi::ICommandBuffer *pointer = commands.get();
                    rhi::IFence *fence = m_fence.fence();
                    const auto value = m_fence.nextValue();
                    const u64 raw = value;
                    rhi::SubmitDesc description{};
                    description.commandBuffers = &pointer;
                    description.commandBufferCount = 1u;
                    description.signalFences = &fence;
                    description.signalFenceValues = &raw;
                    description.signalFenceCount = 1u;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->submit(description)));
                    m_fence.commitSubmitted(value);
                    PPR_TEST_ASSERT(not m_fence.waitFor(value));
                    m_value = value;
                }

                // Runs one pass frame over a scene whose valid rectangle contains
                // `valid_value` and whose backing padding (when present) contains
                // `padding_value`. Returns the resolved output luminance per pixel.
                [[nodiscard]] std::vector<float> run(
                    const int2 valid, const int2 backing, const float valid_value, const float padding_value,
                    const PostProcessSettings &settings) {
                    auto &device = m_rhi->getDevice();
                    std::vector<u16> scene(
                        static_cast<std::size_t>(backing.x) * static_cast<std::size_t>(backing.y) * 4u);
                    for (i32 y = 0; y < backing.y; ++y) {
                        for (i32 x = 0; x < backing.x; ++x) {
                            const bool inside = x < valid.x and y < valid.y;
                            const float value = inside ? valid_value : padding_value;
                            const u16 half = toHalf(value);
                            const std::size_t pixel = static_cast<std::size_t>(y) *
                                                          static_cast<std::size_t>(backing.x) +
                                                      static_cast<std::size_t>(x);
                            scene[pixel * 4u + 0u] = half;
                            scene[pixel * 4u + 1u] = half;
                            scene[pixel * 4u + 2u] = half;
                            scene[pixel * 4u + 3u] = toHalf(1.0f);
                        }
                    }

                    const rhi::SubresourceData initial{
                        .data = scene.data(),
                        .rowPitch = static_cast<std::size_t>(backing.x) * 4u * sizeof(u16),
                        .slicePitch = scene.size() * sizeof(u16)};
                    rhi::TextureDesc scene_desc{};
                    scene_desc.type = rhi::TextureType::Texture2D;
                    scene_desc.size = {
                        .width = static_cast<u32>(backing.x), .height = static_cast<u32>(backing.y), .depth = 1u};
                    scene_desc.arrayLength = 1u;
                    scene_desc.mipCount = 1u;
                    scene_desc.format = rhi::Format::RGBA16Float;
                    scene_desc.usage = rhi::TextureUsage::ShaderResource;
                    scene_desc.defaultState = rhi::ResourceState::ShaderResource;
                    scene_desc.label = "padded post scene";
                    rhi::ComPtr<rhi::ITexture> scene_texture;
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createTexture(scene_desc, &initial, scene_texture.writeRef())));
                    const rhi::ComPtr<rhi::ITextureView> scene_view = scene_texture->getDefaultView();

                    const GridWorldIdentity world{
                        .m_job_id = 1u, .m_world_generation = 1u, .m_content_fingerprint = 1u};
                    PPR_TEST_ASSERT(not m_pass.beginFrame(device, valid, world, m_fence.completed()));

                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                    const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                    const PreRenderContext pre{device, *encoder, m_fence.completed(), {}};
                    const ResolvedHdrView view{
                        .m_view = scene_view,
                        .m_world = world,
                        .m_backing = backing,
                        .m_valid = valid,
                    };
                    PPR_TEST_ASSERT(not m_pass.prepare(pre, view, settings, std::chrono::milliseconds{16}));

                    rhi::ITexture &output = m_pass.outputTexture();
                    const rhi::ComPtr<rhi::ITextureView> output_view = output.getDefaultView();
                    encoder->setTextureState(&output, range, rhi::ResourceState::RenderTarget);
                    const std::array<rhi::RenderPassColorAttachment, 1> colors{
                        rhi::RenderPassColorAttachment{.view = output_view, .clearValue = {0.0f, 0.0f, 0.0f, 1.0f}}};
                    rhi::RenderPassDesc description{};
                    description.colorAttachments = colors.data();
                    description.colorAttachmentCount = 1u;
                    rhi::IRenderPassEncoder *const pass = encoder->beginRenderPass(description);
                    PPR_TEST_ASSERT(pass != nullptr);
                    {
                        RenderPipelineSignature signature{};
                        signature.m_color_formats[0] = rhi::Format::RGBA16Float;
                        signature.m_color_format_count = 1u;
                        signature.m_sample_count = SampleCount::x1;
                        const RenderPipelineKey key(signature);
                        const auto viewport = rhi::Viewport::fromSize(
                            static_cast<float>(valid.x), static_cast<float>(valid.y));
                        const auto scissor = rhi::ScissorRect::fromSize(
                            static_cast<u32>(valid.x), static_cast<u32>(valid.y));
                        const DrawContext context{
                            .m_device = device,
                            .m_pass = *pass,
                            .m_render_pipeline_key = key,
                            .m_viewport = viewport,
                            .m_scissor = scissor,
                            .m_target_extent = valid,
                            .m_completed_fence_value = m_fence.completed(),
                            .m_wait_for_fence = {},
                        };
                        pass->setRenderState({
                            .viewports = {viewport},
                            .viewportCount = 1u,
                            .scissorRects = {scissor},
                            .scissorRectCount = 1u,
                            .vertexBuffers = {},
                            .indexBuffer = {},
                        });
                        PPR_TEST_ASSERT(not m_pass.render(context));
                    }
                    pass->end();
                    encoder->setTextureState(&output, range, rhi::ResourceState::ShaderResource);

                    submit(*encoder);
                    m_pass.notifySubmitted(m_value);
                    m_pass.notifyPresented(m_value);
                    PPR_TEST_ASSERT(not m_pass.collectCompleted(m_value));

                    shader::ComPtr<ISlangBlob> blob;
                    rhi::SubresourceLayout layout{};
                    PPR_TEST_ASSERT(not make_error_code(device.readTexture(&output, 0u, 0u, blob.writeRef(), &layout)));
                    const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer());
                    std::vector<float> result(static_cast<std::size_t>(valid.x) * static_cast<std::size_t>(valid.y));
                    for (i32 y = 0; y < valid.y; ++y) {
                        for (i32 x = 0; x < valid.x; ++x) {
                            u16 half{};
                            std::memcpy(
                                &half,
                                bytes +
                                    static_cast<std::size_t>(y) * layout.rowPitch +
                                    static_cast<std::size_t>(x) * 4u * sizeof(u16),
                                sizeof(half));
                            result
                                [static_cast<std::size_t>(y) * static_cast<std::size_t>(valid.x) +
                                    static_cast<std::size_t>(x)] = fromHalf(half);
                        }
                    }

                    return result;
                }

                void shutdown() {
                    if (m_queue and make_error_code(m_queue->waitOnHost())) {
                        hal::fatalExit("post-backing-test-quiescence-unknown");
                    }

                    PPR_TEST_ASSERT(not m_pass.shutdown());
                    PPR_TEST_ASSERT(not m_fence.shutdown());
                    m_queue.setNull();
                    PPR_TEST_ASSERT(not m_rhi->shutdown());
                    PPR_TEST_ASSERT(not m_shaders->shutdown());
                }
            };

            PPR_UNIT_TEST (gpu_padding_does_not_reach_metering_or_bloom) {
                Capture capture;
                capture.initialize();
                PPR_DEFER {
                    capture.shutdown();
                };

                PostProcessSettings settings{};
                settings.m_auto_exposure = false;
                settings.m_bloom_enabled = false;
                settings.m_exposure_ev = 0.0f;
                const int2 valid{5, 3};

                // The same valid content with no padding, then with black and very
                // bright padding outside R. Both padded results must match the
                // unpadded baseline byte-for-byte.
                const auto baseline = capture.run(valid, valid, 0.25f, 0.0f, settings);
                const auto black_padding = capture.run(valid, int2{8, 8}, 0.25f, 0.0f, settings);
                const auto bright_padding = capture.run(valid, int2{8, 8}, 0.25f, 2048.0f, settings);
                PPR_TEST_ASSERT(baseline.size() == black_padding.size());
                PPR_TEST_ASSERT(baseline.size() == bright_padding.size());
                for (u32 index = 0u; index < baseline.size(); ++index) {
                    PPR_TEST_ASSERT(baseline[index] == black_padding[index]);
                    PPR_TEST_ASSERT(baseline[index] == bright_padding[index]);
                }

                // Bloom enabled: 0.25 is below threshold 1.0, so it contributes
                // nothing. If the 2048 padding were sampled at the R edge, the
                // bright-pass prefilter would produce a visible edge glow.
                PostProcessSettings bloom{};
                bloom.m_auto_exposure = false;
                bloom.m_bloom_enabled = true;
                bloom.m_exposure_ev = 0.0f;
                bloom.m_bloom_threshold = 1.0f;
                const auto bloomed_baseline = capture.run(valid, valid, 0.25f, 0.0f, bloom);
                const auto bloomed_bright_padding = capture.run(valid, int2{8, 8}, 0.25f, 2048.0f, bloom);
                for (u32 index = 0u; index < baseline.size(); ++index) {
                    PPR_TEST_ASSERT(bloomed_baseline[index] == baseline[index]);
                    PPR_TEST_ASSERT(bloomed_bright_padding[index] == bloomed_baseline[index]);
                }
            };

            PPR_UNIT_TEST (gpu_r1_and_odd_non_square_render) {
                Capture capture;
                capture.initialize();
                PPR_DEFER {
                    capture.shutdown();
                };

                PostProcessSettings settings{};
                settings.m_auto_exposure = false;
                settings.m_bloom_enabled = true;
                settings.m_exposure_ev = 0.0f;
                settings.m_bloom_threshold = 1.0f;

                // R=1: one output texel, a single bloom level, no out-of-range
                // fetch or zero-sized chain.
                const auto one = capture.run(int2{1, 1}, int2{1, 1}, 0.25f, 0.0f, settings);
                PPR_TEST_ASSERT(one.size() == 1u);
                PPR_TEST_ASSERT(std::isfinite(one[0]));

                // Odd non-square R=7x3 with a matching padded backing. The valid
                // result must equal the unpadded R=7x3 result.
                const int2 odd{7, 3};
                const auto unpadded = capture.run(odd, odd, 0.25f, 0.0f, settings);
                const auto padded = capture.run(odd, int2{8, 3}, 0.25f, 2048.0f, settings);
                PPR_TEST_ASSERT(unpadded.size() == 21u);
                PPR_TEST_ASSERT(padded.size() == 21u);
                for (u32 index = 0u; index < unpadded.size(); ++index) {
                    PPR_TEST_ASSERT(std::isfinite(unpadded[index]));
                    PPR_TEST_ASSERT(unpadded[index] == padded[index]);
                }
            };
        }

        const UnitTest backing = UnitTest::Named("backing") / [](UnitTest::IRun &run) -> void {
            run.recurse({
                Backing::gpu_padding_does_not_reach_metering_or_bloom,
                Backing::gpu_r1_and_odd_non_square_render,
            });
        };
    }
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // independent post suite
    // ------------------------------------------------------------------

    const UnitTest post_process_contracts = UnitTest::Named("post_process") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::PostProcess::presentation_grid_uses_checked_ceil_arithmetic,
            detail::PostProcess::post_settings_reject_nonfinite_and_invalid_tuples,
            detail::PostProcess::teardown_is_idempotent_before_initialize,
            detail::PostProcess::HistogramAdaptation::histogram_adaptation,
            detail::PostProcess::SpdBloom::spd_bloom,
            detail::PostProcess::output_lifetime,
            detail::PostProcess::backing,
        });
    };

    const UnitTest &post_processTests() noexcept {
        return post_process_contracts;
    }
}
