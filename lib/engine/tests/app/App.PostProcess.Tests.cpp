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
    // post-effect contracts
    // ------------------------------------------------------------------

    namespace PostProcess {
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
                std::vector<float> black(4096u, 0.0f);
                std::array<u32, RetroPass::kHistogramBins> black_hist{};
                for (const float lum: black) {
                    ++black_hist[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
                }
                const float black_avg = RetroPass::averageLog2Luminance(black_hist, static_cast<u32>(black.size()));
                PPR_TEST_ASSERT(std::isfinite(black_avg));
                PPR_TEST_ASSERT(RetroPass::targetEv(black_avg, ev_min, ev_max) == ev_max);

                // Uniform 0.18 middle gray: target EV lands near zero.
                std::vector<float> uniform(4096u, 0.18f);
                std::array<u32, RetroPass::kHistogramBins> uniform_hist{};
                for (const float lum: uniform) {
                    ++uniform_hist[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
                }
                const float uniform_avg = RetroPass::averageLog2Luminance(
                    uniform_hist, static_cast<u32>(uniform.size()));
                const float uniform_ev = RetroPass::targetEv(uniform_avg, ev_min, ev_max);
                PPR_TEST_ASSERT(std::isfinite(uniform_ev));
                PPR_TEST_ASSERT(std::abs(uniform_ev) < 0.25f);

                // Bright-emitter dominated: one emitter among a dark background. The
                // percentile clip suppresses the emitter, leaving the dark background
                // to drive a positive target EV, clamped at the upper bound.
                std::vector<float> emitter(4096u, 0.01f);
                emitter[0] = 1000.0f;
                std::array<u32, RetroPass::kHistogramBins> emitter_hist{};
                for (const float lum: emitter) {
                    ++emitter_hist[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
                }
                const float emitter_avg = RetroPass::averageLog2Luminance(
                    emitter_hist, static_cast<u32>(emitter.size()));
                const float emitter_ev = RetroPass::targetEv(emitter_avg, ev_min, ev_max);
                PPR_TEST_ASSERT(std::isfinite(emitter_ev));
                PPR_TEST_ASSERT(emitter_ev > uniform_ev);
                PPR_TEST_ASSERT(emitter_ev <= ev_max);
            };

            PPR_UNIT_TEST (cpu_adaptation_alpha) {
                // A 60 Hz frame blends a small fraction, never an instant snap.
                const float slow = RetroPass::adaptationAlpha(std::chrono::milliseconds{16}, 0.7f);
                PPR_TEST_ASSERT(slow > 0.0f and slow < 0.1f);

                // A cumulative timestamp (e.g. an hour since startup) would drive the
                // blend to ~1; the adaptation path must never treat request_elapsed as
                // the frame delta, or this is what it would compute.
                const float timestamp = RetroPass::adaptationAlpha(std::chrono::hours{1}, 0.7f);
                PPR_TEST_ASSERT(timestamp > 0.99f);

                // Zero delta and non-positive tau never adapt.
                PPR_TEST_ASSERT(RetroPass::adaptationAlpha(TimeSpan{}, 0.7f) == 0.0f);
                PPR_TEST_ASSERT(RetroPass::adaptationAlpha(std::chrono::milliseconds{16}, 0.0f) == 0.0f);
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
                        device_buffer(RetroPass::kHistogramBins * sizeof(u32), "histogram bins", m_histogram);
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
                            u32 m_source_extent[2];
                            u32 m_luminance_extent[2];
                            u32 m_reserved0;
                            u32 m_reserved1;
                        };

                        const u32 edge = 64u;
                        const HistogramParameters histogram{{edge, edge}, {edge, edge}, 0u, 0u};

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
                            RetroPass::adaptationAlpha(dt, tau), -4.0f, 4.0f, enabled ? 1u : 0u};

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
                    const float alpha = RetroPass::adaptationAlpha(dt, tau);

                    Fixture fixture;
                    fixture.initialize(0.18f);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    // CPU oracle target for the uniform 0.18 scene (64x64 luminance).
                    // The GPU reduction writes the unclamped target EV; the final clamp
                    // happens in adaptExposure, so compare against the unclamped value.
                    std::vector<float> uniform(std::size_t(64u) * 64u, 0.18f);
                    std::array<u32, RetroPass::kHistogramBins> histogram{};
                    for (const float lum: uniform) {
                        ++histogram[RetroPass::luminanceBin(RetroPass::log2Luminance(lum))];
                    }
                    const float cpu_target = std::log2(RetroPass::kMiddleGrayLuminance) -
                                             RetroPass::averageLog2Luminance(
                                                 histogram, static_cast<u32>(uniform.size()));

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
                // brighter output after the present-side exp2 multiply. This does not
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
                    {cpu_histogram_math, cpu_adaptation_alpha, Gpu::gpu_adaptation_smoke,
                        Gpu::gpu_dark_scene_target_ev_raises_exposure});
            };
        }

        // ------------------------------------------------------------------
        // SPD-style bloom
        // ------------------------------------------------------------------

        namespace SpdBloom {
            PPR_UNIT_TEST (cpu_bloom_chain_layout) {
                // A 1080p picture: half-res mip 0, six mips, floor-halved extents.
                const int2 mip0 = RetroPass::bloomMip0Extent(int2{1920, 1080});
                PPR_TEST_ASSERT(mip0.x == 960 and mip0.y == 540);
                PPR_TEST_ASSERT(RetroPass::bloomMipCount(mip0) == 6u);
                PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 0u).x == 960);
                PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 1u).x == 480);
                PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 5u).x == 30);
                PPR_TEST_ASSERT(RetroPass::bloomMipExtent(mip0, 5u).y == 16);

                // Cumulative RGBA16F bytes (8 per texel) over the six-mip chain.
                const u64 expected =
                    (960ull * 540 + 480ull * 270 + 240ull * 135 + 120ull * 67 + 60ull * 33 + 30ull * 16) * 8u;
                PPR_TEST_ASSERT(RetroPass::bloomChainBytes(mip0) == expected);

                // Odd extents stay floor-halved and clamped at one texel.
                const int2 odd = RetroPass::bloomMip0Extent(int2{101, 51});
                PPR_TEST_ASSERT(odd.x == 51 and odd.y == 26);
                PPR_TEST_ASSERT(RetroPass::bloomMipExtent(odd, 0u).y == 26);
                PPR_TEST_ASSERT(RetroPass::bloomMipExtent(odd, 5u).x == 1);
                PPR_TEST_ASSERT(RetroPass::bloomMipExtent(odd, 31u).y == 1);

                // A tiny chain stays a single mip (still capped at six, never zero).
                PPR_TEST_ASSERT(RetroPass::bloomMipCount(int2{1, 1}) == 1u);
            };

            namespace Gpu {
                struct BloomParams {
                    u32 m_source[2];
                    u32 m_mip0[2];
                    float m_threshold;
                    float m_soft_knee;
                    u32 m_mip_count;
                    u32 m_upsample_level;
                };
                static_assert(sizeof(BloomParams) == 32u);

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

                    rhi::ComPtr<rhi::ITexture> m_bloom;
                    rhi::ComPtr<rhi::ITextureView> m_bloom_view;
                    std::array<rhi::ComPtr<rhi::ITextureView>, RetroPass::kMaxBloomMips> m_bloom_uav_views{};
                    std::array<rhi::ComPtr<rhi::IShaderProgram>, 3> m_programs{};
                    std::array<rhi::ComPtr<rhi::IComputePipeline>, 3> m_pipelines{};
                    std::array<rhi::ComPtr<rhi::IShaderObject>, 3> m_roots{};

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

                        const int2 mip0 = RetroPass::bloomMip0Extent(int2{int(edge), int(edge)});
                        const u32 mip_count = RetroPass::bloomMipCount(mip0);
                        rhi::TextureDesc bloom_desc{};
                        bloom_desc.type = rhi::TextureType::Texture2D;
                        bloom_desc.size = {.width = u32(mip0.x), .height = u32(mip0.y), .depth = 1u};
                        bloom_desc.arrayLength = 1u;
                        bloom_desc.mipCount = mip_count;
                        bloom_desc.format = rhi::Format::RGBA16Float;
                        bloom_desc.usage = static_cast<rhi::TextureUsage>(
                            static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                            static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
                        bloom_desc.defaultState = rhi::ResourceState::UnorderedAccess;
                        bloom_desc.label = "spd bloom chain";
                        PPR_TEST_ASSERT(
                            not make_error_code(device.createTexture(bloom_desc, nullptr, m_bloom.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(m_bloom->getDefaultView(m_bloom_view.writeRef())));
                        for (u32 level = 0u; level < mip_count; ++level) {
                            rhi::TextureViewDesc view_desc{};
                            view_desc.subresourceRange = {.layer = 0u, .layerCount = 1u, .mip = level, .mipCount = 1u};
                            PPR_TEST_ASSERT(not make_error_code(
                                m_bloom->createView(view_desc, m_bloom_uav_views[level].writeRef())));
                        }

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
                    // returns the accumulated mip 0 decoded to linear float RGB.
                    // adapted_ev is the 1x1 exposure offset the prefilter samples
                    // (0 = fixed exposure).
                    std::vector<std::array<float, 3>> run(
                        const u32 edge, const float background, const float block_value,
                        const float adapted_ev = 0.0f) {
                        const int2 mip0 = RetroPass::bloomMip0Extent(int2{int(edge), int(edge)});
                        const u32 mip_count = RetroPass::bloomMipCount(mip0);
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
                        scene_desc.label = "spd bloom synthetic scene";
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
                        exposure_desc.label = "spd bloom exposure offset";
                        rhi::ComPtr<rhi::ITexture> exposure_tex;
                        rhi::ComPtr<rhi::ITextureView> exposure_view;
                        PPR_TEST_ASSERT(not make_error_code(
                            device.createTexture(exposure_desc, &exposure_initial, exposure_tex.writeRef())));
                        PPR_TEST_ASSERT(not make_error_code(exposure_tex->getDefaultView(exposure_view.writeRef())));

                        BloomParams params{};
                        params.m_source[0] = edge;
                        params.m_source[1] = edge;
                        params.m_mip0[0] = u32(mip0.x);
                        params.m_mip0[1] = u32(mip0.y);
                        params.m_threshold = 1.0f;
                        params.m_soft_knee = RetroPass::kBloomSoftKnee;
                        params.m_mip_count = mip_count;
                        params.m_upsample_level = 0u;

                        static constexpr std::array<const char *, RetroPass::kMaxBloomMips> kMipNames{
                            "g_bloom_mip0", "g_bloom_mip1", "g_bloom_mip2", "g_bloom_mip3", "g_bloom_mip4",
                            "g_bloom_mip5"};

                        rhi::ComPtr<rhi::ICommandEncoder> encoder;
                        PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));

                        {
                            rhi::ShaderCursor cursor{m_roots[0]}, constants{};
                            PPR_TEST_ASSERT(not make_error_code(cursor["g_bloom"].getDereferenced(constants)));
                            PPR_TEST_ASSERT(not make_error_code(constants.setData(&params, sizeof(params))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_scene"].setBinding(rhi::Binding(scene_view))));
                            PPR_TEST_ASSERT(
                                not make_error_code(cursor["g_exposure"].setBinding(rhi::Binding(exposure_view))));
                            PPR_TEST_ASSERT(not make_error_code(
                                cursor["g_bloom_mip0"].setBinding(rhi::Binding(m_bloom_uav_views[0]))));
                        }
                        {
                            rhi::ShaderCursor cursor{m_roots[1]}, constants{};
                            PPR_TEST_ASSERT(not make_error_code(cursor["g_bloom"].getDereferenced(constants)));
                            PPR_TEST_ASSERT(not make_error_code(constants.setData(&params, sizeof(params))));
                            for (u32 level = 0u; level < RetroPass::kMaxBloomMips; ++level) {
                                const u32 view = std::min(level, mip_count - 1u);
                                PPR_TEST_ASSERT(not make_error_code(
                                    cursor[kMipNames[level]].setBinding(rhi::Binding(m_bloom_uav_views[view]))));
                            }
                        }
                        rhi::ShaderCursor up_cursor{m_roots[2]}, up_constants{};
                        PPR_TEST_ASSERT(not make_error_code(up_cursor["g_bloom"].getDereferenced(up_constants)));

                        const rhi::SubresourceRange chain{
                            .layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = mip_count};
                        encoder->setTextureState(m_bloom, chain, rhi::ResourceState::UnorderedAccess);

                        const auto dispatch = [&](const u32 program, const u32 x, const u32 y) {
                            auto *const pass = encoder->beginComputePass();
                            PPR_TEST_ASSERT(pass != nullptr);
                            pass->bindPipeline(m_pipelines[program], m_roots[program]);
                            pass->dispatchCompute(x, y, 1u);
                            pass->end();
                        };
                        dispatch(0u, (u32(mip0.x) + 7u) / 8u, (u32(mip0.y) + 7u) / 8u);
                        encoder->globalBarrier();
                        if (mip_count > 1u) {
                            dispatch(1u, (u32(mip0.x) + 31u) / 32u, (u32(mip0.y) + 31u) / 32u);
                        }
                        for (u32 level = mip_count - 1u; level >= 1u; --level) {
                            const int2 dst = RetroPass::bloomMipExtent(mip0, level - 1u);
                            params.m_upsample_level = level;
                            PPR_TEST_ASSERT(not make_error_code(up_constants.setData(&params, sizeof(params))));
                            PPR_TEST_ASSERT(not make_error_code(
                                up_cursor["g_up_src"].setBinding(rhi::Binding(m_bloom_uav_views[level]))));
                            PPR_TEST_ASSERT(not make_error_code(
                                up_cursor["g_up_dst"].setBinding(rhi::Binding(m_bloom_uav_views[level - 1u]))));
                            encoder->globalBarrier();
                            dispatch(2u, (u32(dst.x) + 7u) / 8u, (u32(dst.y) + 7u) / 8u);
                        }

                        encoder->setTextureState(m_bloom, chain, rhi::ResourceState::ShaderResource);
                        submit(*encoder);

                        shader::ComPtr<ISlangBlob> blob;
                        rhi::SubresourceLayout layout{};
                        PPR_TEST_ASSERT(
                            not make_error_code(device.readTexture(m_bloom, 0u, 0u, blob.writeRef(), &layout)));
                        const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer());

                        std::vector<std::array<float, 3>> result(std::size_t(mip0.x) * std::size_t(mip0.y));
                        for (u32 y = 0u; y < u32(mip0.y); ++y) {
                            for (u32 x = 0u; x < u32(mip0.x); ++x) {
                                const std::byte *const pixel = bytes + y * layout.rowPitch + x * 8u;
                                std::array<u16, 4> halves{};
                                std::memcpy(halves.data(), pixel, sizeof(halves));
                                result[std::size_t(y) * std::size_t(mip0.x) + x] = {
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
                            hal::fatalExit("spd-bloom-test-quiescence-unknown");

                        m_roots = {};
                        m_pipelines = {};
                        m_programs = {};
                        m_bloom_uav_views = {};
                        m_bloom_view.setNull();
                        m_bloom.setNull();
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

                    const int2 mip0 = RetroPass::bloomMip0Extent(int2{int(edge), int(edge)});
                    const auto at = [&](const std::vector<std::array<float, 3>> &result, const u32 x, const u32 y) {
                        return luminanceOf(result[std::size_t(y) * u32(mip0.x) + x]);
                    };

                    const auto bright = fixture.run(edge, 0.05f, 20.0f);
                    PPR_TEST_ASSERT(bright.size() == std::size_t(mip0.x) * std::size_t(mip0.y));

                    // Finite everywhere, and the bright block leaves a non-zero trail.
                    bool all_finite = true;
                    for (const auto &pixel: bright) {
                        all_finite = all_finite and
                                     std::isfinite(pixel[0]) and
                                     std::isfinite(pixel[1]) and
                                     std::isfinite(pixel[2]);
                    }
                    PPR_TEST_ASSERT(all_finite);

                    // The block is 16 scene texels centred at edge/2, so in mip 0 it
                    // spans (edge/4-2 .. edge/4+1); its centre is clearly lit, the
                    // texel just outside is lit ("around the block"), and a texel
                    // seven away is still lit — far beyond the two-texel reach of a
                    // single 5x5 binomial blur, which a wider multi-mip chain reaches.
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

                    // A uniform bright picture must produce a bounded, non-zero bloom:
                    // the per-level weights keep the peak far below the unweighted ~6x
                    // accumulation (the prefilter of a uniform 20.0 picture is ~0.95).
                    const auto bloomed = fixture.run(edge, 20.0f, 20.0f, 0.0f);
                    float peak = 0.0f;
                    for (const auto &pixel: bloomed) {
                        peak = std::max(peak, luminanceOf(pixel));
                        PPR_TEST_ASSERT(
                            std::isfinite(pixel[0]) and std::isfinite(pixel[1]) and std::isfinite(pixel[2]));
                    }
                    PPR_TEST_ASSERT(peak > 0.5f);
                    PPR_TEST_ASSERT(peak < 4.0f);
                };

                PPR_UNIT_TEST (gpu_bloom_single_mip_not_halved) {
                    constexpr u32 edge = 2u;

                    Fixture fixture;
                    fixture.initialize(edge);
                    PPR_DEFER {
                        fixture.shutdown();
                    };

                    const int2 mip0 = RetroPass::bloomMip0Extent(int2{int(edge), int(edge)});
                    PPR_TEST_ASSERT(mip0.x == 1 and mip0.y == 1);
                    PPR_TEST_ASSERT(RetroPass::bloomMipCount(mip0) == 1u);

                    // A uniform bright picture through a single-mip chain must keep the
                    // prefilter intact: the (skipped) downsample must not alias mip 1
                    // onto mip 0 and halve it.
                    const auto bloomed = fixture.run(edge, 20.0f, 20.0f, 0.0f);
                    PPR_TEST_ASSERT(bloomed.size() == 1u);
                    const float peak = luminanceOf(bloomed[0]);
                    PPR_TEST_ASSERT(std::isfinite(peak));
                    // The prefilter of a uniform 20.0 picture is ~0.95 (Karis-damped);
                    // halving it would land near 0.47, so a value above 0.7 proves the
                    // single mip survives.
                    PPR_TEST_ASSERT(peak > 0.7f);
                };
            }

            const UnitTest spd_bloom = UnitTest::Named("spd_bloom") / [](UnitTest::IRun &run) -> void {
                run.recurse(
                    {cpu_bloom_chain_layout, Gpu::gpu_bloom_smoke, Gpu::gpu_bloom_exposure_aware,
                        Gpu::gpu_bloom_bounded_uniform, Gpu::gpu_bloom_single_mip_not_halved});
            };
        }
    }
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // post-effect contracts suite
    // ------------------------------------------------------------------

    const UnitTest post_process_contracts = UnitTest::Named("post_process") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::PostProcess::HistogramAdaptation::histogram_adaptation,
            detail::PostProcess::SpdBloom::spd_bloom,
        });
    };

    const UnitTest &post_processTests() noexcept {
        return post_process_contracts;
    }
}
