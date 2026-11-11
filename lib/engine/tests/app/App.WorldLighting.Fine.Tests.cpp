module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <slang.h>

module engine.tests.app;

import std;
import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP::tests::detail::WorldLightingFine {
    // ------------------------------------------------------------------
    // fixture values
    // ------------------------------------------------------------------

    inline constexpr u32 kEdge = WorldLightingPass::kWorldEdge;
    inline constexpr u32 kCells = WorldLightingPass::kCellCount;
    inline constexpr u32 kFineEdge = lighting::kFineCHEdge;
    inline constexpr u32 kMaterialCount = 4u;

    inline constexpr u32 kMetal = 1u;
    inline constexpr u32 kDiffuse = 2u;
    inline constexpr u32 kOpaque = 3u;

    // Y0 = 1/sqrt(2*pi), used only by independent CPU oracles here.
    inline constexpr double kY0 = 0.39894228040143267794;

    static_assert(kFineEdge == 512u);
    static_assert(lighting::kFineCHRecordCount == 262'144u);
    static_assert(lighting::kFineCHBytes == 8'388'608u);
    static_assert(sizeof(lighting::PackedCH) == 32u);
    static_assert(WorldLightingPass::kFineDispatchThreads == 8u);
    static_assert(kNumericStatusRecords == 8u);
    static_assert(kNumericStageFine == 7u);

    constexpr u32 cellIndex(const u32 x, const u32 y) noexcept {
        return y * kEdge + x;
    }

    [[nodiscard]] float3 rgb(const float r, const float g, const float b) noexcept {
        return float3{r, g, b};
    }

    [[nodiscard]] TimeSpan activeTime(const double seconds) noexcept {
        return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>(seconds));
    }

    // RGB9E5 shares one exponent, so a small channel carries the shared quantum
    // of the largest component. Comparisons use that quantum.
    [[nodiscard]] bool closeRGB9E5(
        const float3 actual, const float3 expected, const float relative = 4.0e-3f) noexcept {
        const float scale = (std::max)({
            std::abs(actual.x), std::abs(actual.y), std::abs(actual.z), std::abs(expected.x), std::abs(expected.y),
            std::abs(expected.z)});
        const float tolerance = relative * scale + 1.0e-6f;
        return std::abs(actual.x - expected.x) <= tolerance and
               std::abs(actual.y - expected.y) <= tolerance and
               std::abs(actual.z - expected.z) <= tolerance;
    }

    // ------------------------------------------------------------------
    // device fixture
    // ------------------------------------------------------------------

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue{};

        void initialize() {
            PPR_TEST_ASSERT(not m_shaders->initialize());
#ifdef _WIN32
            constexpr auto backend = rhi::DeviceType::D3D12;
#else
            constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
            PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
            PPR_TEST_ASSERT(m_rhi->getDevice().getDeviceType() == backend);
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            const rhi::DeviceInfo &info = m_rhi->getDevice().getInfo();
            std::println(
                "WORLD_LIGHTING_FINE api={} adapter={} backend={}", info.apiName != nullptr ? info.apiName : "unknown",
                info.adapterName != nullptr ? info.adapterName : "unknown", static_cast<int>(info.deviceType));
        }

        void shutdown() {
            m_queue.setNull();
            PPR_TEST_ASSERT(not m_rhi->shutdown());
            PPR_TEST_ASSERT(not m_shaders->shutdown());
        }

        [[nodiscard]] rhi::IDevice &device() const {
            return m_rhi->getDevice();
        }
    };

    // ------------------------------------------------------------------
    // buffer and submission plumbing
    // ------------------------------------------------------------------

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeShaderBuffer(
        rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial = nullptr) {
        rhi::BufferDesc description{};
        description.size = std::max<u64>(bytes, stride);
        description.elementSize = stride;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = rhi::BufferUsage::ShaderResource;
        description.defaultState = rhi::ResourceState::ShaderResource;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, initial, buffer.writeRef())));
        return buffer;
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeStorageBuffer(
        rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial = nullptr) {
        rhi::BufferDesc description{};
        description.size = std::max<u64>(bytes, stride);
        description.elementSize = stride;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::ShaderResource) |
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
            static_cast<u32>(rhi::BufferUsage::CopySource));
        description.defaultState = rhi::ResourceState::UnorderedAccess;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, initial, buffer.writeRef())));
        return buffer;
    }

    [[nodiscard]] rhi::ComPtr<rhi::ICommandEncoder> makeEncoder(Fixture &fixture) {
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        return encoder;
    }

    void submit(Fixture &fixture, rhi::ICommandEncoder &encoder) {
        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
    }

    [[nodiscard]] std::vector<u32> readWords(
        Fixture &fixture, rhi::IBuffer *const buffer, const u64 offset_bytes, const u64 size_bytes) {
        PPR_TEST_ASSERT(buffer != nullptr);
        std::vector<u32> words(static_cast<std::size_t>(size_bytes / sizeof(u32)));
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().readBuffer(buffer, offset_bytes, size_bytes, words.data())));
        return words;
    }

    // ------------------------------------------------------------------
    // categorical map, palette and source inputs
    // ------------------------------------------------------------------

    struct CategoricalMap {
        std::vector<u32> m_words{};

        CategoricalMap() {
            m_words.assign(kCells, 0u);
        }

        void occupy(const u32 x, const u32 y, const u32 material, const double kelvin = 300.0) {
            PPR_TEST_ASSERT(x < kEdge and y < kEdge);
            const auto word = lighting::packCell(static_cast<u16>(material), kelvin);
            PPR_TEST_ASSERT(word.has_value());
            m_words[cellIndex(x, y)] = *word;
        }

        void fill(const u32 material, const double kelvin = 300.0) {
            const auto word = lighting::packCell(static_cast<u16>(material), kelvin);
            PPR_TEST_ASSERT(word.has_value());
            m_words.assign(kCells, *word);
        }

        void fillRect(
            const u32 x0, const u32 y0, const u32 x1, const u32 y1, const u32 material, const double kelvin = 300.0) {
            for (u32 y = y0; y <= y1; ++y) {
                for (u32 x = x0; x <= x1; ++x) {
                    occupy(x, y, material, kelvin);
                }
            }
        }
    };

    [[nodiscard]] std::array<GridTerrainVisual, kMaterialCount> makePalette() {
        std::array<GridTerrainVisual, kMaterialCount> palette{};
        for (u32 index = 0u; index < palette.size(); ++index) {
            palette[index].m_material_id = index;
            palette[index].m_color_rgba = 0xffff'ffffu;
            palette[index].m_opacity = 1.0f;
            palette[index].m_ior = 1.5f;
            palette[index].m_filtered_mean_rgb[0] = 1.0f;
            palette[index].m_filtered_mean_rgb[1] = 1.0f;
            palette[index].m_filtered_mean_rgb[2] = 1.0f;
        }

        palette[kMetal].m_metallic = 1.0f;
        palette[kDiffuse].m_metallic = 0.0f;
        return palette;
    }

    [[nodiscard]] std::array<lighting::SourceMaterial, kMaterialCount> uniformSources(
        const float3 emission, const float metallic = 1.0f) {
        std::array<lighting::SourceMaterial, kMaterialCount> sources{};
        const auto record = lighting::makeSourceMaterial(
            emission, 1.0, rgb(1.0f, 1.0f, 1.0f), rgb(1.0f, 1.0f, 1.0f), metallic);
        PPR_TEST_ASSERT(record.has_value());
        for (auto &source: sources) {
            source = *record;
        }

        return sources;
    }

    struct FineInputs {
        const std::array<GridTerrainVisual, kMaterialCount> &m_visuals;
        const CategoricalMap &m_map;
        rhi::ComPtr<rhi::IBuffer> m_palette{};
        rhi::ComPtr<rhi::IBuffer> m_terrain{};
        rhi::ComPtr<rhi::IBuffer> m_thermal{};
        rhi::ComPtr<rhi::IBuffer> m_sources{};

        void uploadCommon(Fixture &fixture) {
            m_palette = makeShaderBuffer(
                fixture.device(), sizeof(m_visuals), sizeof(GridTerrainVisual), m_visuals.data());
            m_terrain = makeShaderBuffer(
                fixture.device(), m_map.m_words.size() * sizeof(u32), sizeof(u32), m_map.m_words.data());

            const std::vector<u8> zero_thermal(lighting::kThermalLutBytes, 0u);
            m_thermal = makeShaderBuffer(
                fixture.device(), zero_thermal.size(), lighting::kThermalLutStride, zero_thermal.data());
        }

        void setSources(Fixture &fixture, const std::array<lighting::SourceMaterial, kMaterialCount> &sources) {
            m_sources = makeShaderBuffer(
                fixture.device(), sizeof(sources), sizeof(lighting::SourceMaterial), sources.data());
        }

        [[nodiscard]] lighting::WorldLightingInputs inputs() const {
            lighting::WorldLightingInputs result{};
            result.m_terrain = m_terrain;
            result.m_visible_materials = m_palette;
            result.m_source_materials = m_sources;
            result.m_thermal_lut = m_thermal;
            result.m_material_count = kMaterialCount;
            return result;
        }
    };

    // ------------------------------------------------------------------
    // attempt driving
    // ------------------------------------------------------------------

    void runFrame(
        Fixture &fixture, WorldLightingPass &pass, const lighting::WorldLightingInputs &inputs, const u64 accepted,
        const double active_seconds, const i32 hour, const FenceCompletionValue fence) {
        pass.setAcceptedFrameForTest(accepted);

        WorldLightingFrame frame{};
        frame.m_active_elapsed = activeTime(active_seconds);
        frame.m_sky_hour = hour;

        auto encoder = makeEncoder(fixture);
        PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
        PPR_TEST_ASSERT(not pass.prepare(context, inputs, frame));
        PPR_TEST_ASSERT(not pass.finalize(context));
        submit(fixture, *encoder);

        pass.notifySubmitted(fence);
        PPR_TEST_ASSERT(pass.acceptedFrameForTest() == accepted + 1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(fence));
    }

    // Encodes a definitely-unsubmitted attempt and releases its slot. Nothing
    // reaches the GPU: phase, field validity and the timing baseline must not
    // move.
    void dropAttempt(
        Fixture &fixture, WorldLightingPass &pass, const lighting::WorldLightingInputs &inputs, const u64 accepted,
        const double active_seconds, const i32 hour) {
        pass.setAcceptedFrameForTest(accepted);

        WorldLightingFrame frame{};
        frame.m_active_elapsed = activeTime(active_seconds);
        frame.m_sky_hour = hour;

        auto encoder = makeEncoder(fixture);
        PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
        PPR_TEST_ASSERT(not pass.prepare(context, inputs, frame));
        PPR_TEST_ASSERT(not pass.finalize(context));
        pass.discardUnsubmitted();
    }

    // Encodes and submits one accepted attempt without asserting a clean
    // collection; the caller settles the status expectations.
    void submitAttempt(
        Fixture &fixture, WorldLightingPass &pass, const lighting::WorldLightingInputs &inputs, const u64 accepted,
        const double active_seconds, const i32 hour, const FenceCompletionValue fence) {
        pass.setAcceptedFrameForTest(accepted);

        WorldLightingFrame frame{};
        frame.m_active_elapsed = activeTime(active_seconds);
        frame.m_sky_hour = hour;

        auto encoder = makeEncoder(fixture);
        PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
        PPR_TEST_ASSERT(not pass.prepare(context, inputs, frame));
        PPR_TEST_ASSERT(not pass.finalize(context));
        submit(fixture, *encoder);
        pass.notifySubmitted(fence);
        PPR_TEST_ASSERT(pass.acceptedFrameForTest() == accepted + 1u);
    }

    // ------------------------------------------------------------------
    // fine-field access and independent CH oracles
    // ------------------------------------------------------------------

    [[nodiscard]] lighting::PackedCH readFineRecord(
        Fixture &fixture, WorldLightingPass &pass, const u32 x, const u32 y) {
        PPR_TEST_ASSERT(pass.fineCHForTest() != nullptr);
        const u64 offset = static_cast<u64>(y * kFineEdge + x) * sizeof(lighting::PackedCH);
        const auto words = readWords(fixture, pass.fineCHForTest(), offset, sizeof(lighting::PackedCH));

        lighting::PackedCH record{};
        std::memcpy(record.m_words.data(), words.data(), sizeof(record));
        return record;
    }

    [[nodiscard]] float3 fineAngularMean(const lighting::PackedCH &record) {
        return lighting::unpackCH(record).m_coefficients[0] * static_cast<float>(kY0);
    }

    [[nodiscard]] double sinc(const double x) noexcept {
        return x == 0.0 ? 1.0 : std::sin(x) / x;
    }

    [[nodiscard]] double lanczos(const u32 m) noexcept {
        return sinc(static_cast<double>(m) * std::numbers::pi / 5.0);
    }

    // Independent analytic field and fit oracle. Coefficients are the
    // orthonormal circular-basis values; the fit applies the Lanczos window once.
    struct HOracle {
        std::array<float3, 9u> m_coefficients;

        HOracle() noexcept {
            for (auto &coefficient: m_coefficients) {
                coefficient = float3{0.0f, 0.0f, 0.0f};
            }
        }

        [[nodiscard]] float3 evaluate(const double theta) const noexcept {
            const double y0 = 1.0 / std::sqrt(2.0 * std::numbers::pi);
            const double inverse_root_pi = 1.0 / std::sqrt(std::numbers::pi);
            double3 value{
                static_cast<double>(m_coefficients[0].x) * y0, static_cast<double>(m_coefficients[0].y) * y0,
                static_cast<double>(m_coefficients[0].z) * y0};
            for (u32 m = 1u; m <= 4u; ++m) {
                const double cosine = std::cos(static_cast<double>(m) * theta) * inverse_root_pi;
                const double sine = std::sin(static_cast<double>(m) * theta) * inverse_root_pi;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const auto &cc = m_coefficients[2u * m - 1u];
                    const auto &cs = m_coefficients[2u * m];
                    const double c = channel == 0u
                                         ? static_cast<double>(cc.x)
                                         : (channel == 1u ? static_cast<double>(cc.y) : static_cast<double>(cc.z));
                    const double s = channel == 0u
                                         ? static_cast<double>(cs.x)
                                         : (channel == 1u ? static_cast<double>(cs.y) : static_cast<double>(cs.z));
                    const double component = channel == 0u ? value.x : (channel == 1u ? value.y : value.z);
                    const double updated = component + static_cast<double>(c) * cosine + static_cast<double>(s) * sine;
                    if (channel == 0u)
                        value.x = updated;
                    else if (channel == 1u)
                        value.y = updated;
                    else
                        value.z = updated;
                }
            }

            return rgb(static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z));
        }

        [[nodiscard]] std::array<float3, 9u> windowed() const noexcept {
            std::array<float3, 9u> result = m_coefficients;
            for (u32 m = 1u; m <= 4u; ++m) {
                const float window = static_cast<float>(lanczos(m));
                result[2u * m - 1u] = result[2u * m - 1u] * window;
                result[2u * m] = result[2u * m] * window;
            }

            return result;
        }
    };

    // Rotated analytic field: coefficients rotate by m*delta in the same basis.
    [[nodiscard]] std::array<float3, 9u> rotatedCoefficients(
        const std::array<float3, 9u> &coefficients, const double delta) noexcept {
        std::array<float3, 9u> result = coefficients;
        for (u32 m = 1u; m <= 4u; ++m) {
            const double angle = static_cast<double>(m) * delta;
            const float cosine = static_cast<float>(std::cos(angle));
            const float sine = static_cast<float>(std::sin(angle));
            const float3 cc = coefficients[2u * m - 1u];
            const float3 cs = coefficients[2u * m];
            result[2u * m - 1u] = cc * cosine - cs * sine;
            result[2u * m] = cc * sine + cs * cosine;
        }

        return result;
    }

    // ------------------------------------------------------------------
    // probe programs
    // ------------------------------------------------------------------

    struct CHProbeCase {
        u32 m_sample_offset{};
        u32 m_sample_count{};
        float m_current_weight{};
        u32 m_first_field{};
        std::array<u32, 8u> m_previous{};
    };

    struct CHProbeSample {
        float3 m_radiance;
        float m_theta{};
        float m_weight{};
        std::array<float, 3u> m_reserved{};
    };

    static_assert(sizeof(CHProbeCase) == 48u);
    static_assert(sizeof(CHProbeSample) == 32u);

    struct L1ProbeQuery {
        u32 m_probe_x{};
        u32 m_probe_y{};
        u32 m_phi{};
        u32 m_reserved{};
    };

    struct ProbeRunner {
        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        rhi::ComPtr<rhi::IShaderObject> m_root{};
    };

    [[nodiscard]] ProbeRunner makeProbe(Fixture &fixture, const char *const entry) {
        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_fine_probe.slang",
            "world_lighting_fine_probe", module.writeRef()));

        shader::ComPtr<slang::IEntryPoint> point{};
        PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entry, point.writeRef())));

        ProbeRunner result{};
        slang::IComponentType *entry_points[]{point.get()};
        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = module.get();
        description.slangEntryPoints = entry_points;
        description.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics;
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().createShaderProgram(description, result.m_program.writeRef(), diagnostics.writeRef())));

        rhi::ComputePipelineDesc pipeline{};
        pipeline.program = result.m_program;
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createComputePipeline(pipeline, result.m_pipeline.writeRef())));
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createRootShaderObject(result.m_program, result.m_root.writeRef())));
        return result;
    }

    [[nodiscard]] rhi::BufferRange wholeRange(rhi::IBuffer *const buffer) noexcept {
        return {.offset = 0u, .size = buffer->getDesc().size};
    }

    void bindBuffer(rhi::ShaderCursor &cursor, const char *const name, rhi::IBuffer *const buffer) {
        auto field = cursor[name];
        PPR_TEST_ASSERT(field.isValid());
        PPR_TEST_ASSERT(not make_error_code(field.setBinding(rhi::Binding(buffer, wholeRange(buffer)))));
    }

    void bindOptionalBuffer(rhi::ShaderCursor &cursor, const char *const name, rhi::IBuffer *const buffer) {
        auto field = cursor[name];
        if (not field.isValid()) {
            return;
        }

        PPR_TEST_ASSERT(not make_error_code(field.setBinding(rhi::Binding(buffer, wholeRange(buffer)))));
    }

    template<typename T>
    void bindConstant(rhi::ShaderCursor &cursor, const char *const name, const T &value) {
        auto field = cursor[name];
        PPR_TEST_ASSERT(field.isValid());
        rhi::ShaderCursor contents;
        PPR_TEST_ASSERT(not make_error_code(field.getDereferenced(contents)));
        PPR_TEST_ASSERT(not make_error_code(contents.setData(&value, sizeof(value))));
    }

    template<typename T>
    void bindOptionalConstant(rhi::ShaderCursor &cursor, const char *const name, const T &value) {
        auto field = cursor[name];
        if (not field.isValid()) {
            return;
        }

        rhi::ShaderCursor contents;
        PPR_TEST_ASSERT(not make_error_code(field.getDereferenced(contents)));
        PPR_TEST_ASSERT(not make_error_code(contents.setData(&value, sizeof(value))));
    }

    void dispatchProbe(Fixture &fixture, ProbeRunner &probe, const u32 groups) {
        auto encoder = makeEncoder(fixture);
        auto *const pass = encoder->beginComputePass();
        PPR_TEST_ASSERT(pass != nullptr);
        {
            PPR_DEFER {
                pass->end();
            };
            pass->bindPipeline(probe.m_pipeline, probe.m_root);
            pass->dispatchCompute(groups, 1u, 1u);
        }
        submit(fixture, *encoder);
    }

    // Runs the real CH fit/encode helpers for synthetic sample sets.
    [[nodiscard]] std::vector<std::array<u32, 9u>> runCHProbe(
        Fixture &fixture, const std::vector<CHProbeCase> &cases, const std::vector<CHProbeSample> &samples) {
        PPR_TEST_ASSERT(not cases.empty());
        PPR_TEST_ASSERT(not samples.empty());

        ProbeRunner probe = makeProbe(fixture, "probeCH");
        auto case_buffer = makeShaderBuffer(
            fixture.device(), cases.size() * sizeof(CHProbeCase), sizeof(CHProbeCase), cases.data());
        auto sample_buffer = makeShaderBuffer(
            fixture.device(), samples.size() * sizeof(CHProbeSample), sizeof(CHProbeSample), samples.data());
        auto result_buffer = makeStorageBuffer(fixture.device(), cases.size() * 9u * sizeof(u32), sizeof(u32));

        rhi::ShaderCursor cursor{probe.m_root};
        bindBuffer(cursor, "g_ch_cases", case_buffer);
        bindBuffer(cursor, "g_ch_samples", sample_buffer);
        bindBuffer(cursor, "g_ch_results", result_buffer);
        bindOptionalBuffer(cursor, "g_fine_ch", result_buffer);
        bindOptionalConstant(cursor, "g_fine", std::array<u32, 8u>{});
        const std::array<u32, 4u> count{static_cast<u32>(cases.size()), 0u, 0u, 0u};
        bindConstant(cursor, "g_ch_probe", count);

        dispatchProbe(fixture, probe, 1u);

        const auto words = readWords(fixture, result_buffer, 0u, cases.size() * 9u * sizeof(u32));
        std::vector<std::array<u32, 9u>> results(cases.size());
        for (std::size_t index = 0u; index < results.size(); ++index) {
            std::copy_n(words.begin() + static_cast<std::ptrdiff_t>(index * 9u), 9u, results[index].begin());
        }

        return results;
    }

    struct L1ProbeResult {
        u32 m_phase{};
        bool m_valid{};
        u32 m_trace_low{};
        u32 m_trace_high{};
        float2 m_direction;
        u32 m_age{};
    };

    [[nodiscard]] std::vector<L1ProbeResult> runL1Probe(
        Fixture &fixture, const std::vector<L1ProbeQuery> &queries, const u64 accepted_frame) {
        PPR_TEST_ASSERT(not queries.empty());

        ProbeRunner probe = makeProbe(fixture, "probeL1Frames");
        auto query_buffer = makeShaderBuffer(
            fixture.device(), queries.size() * sizeof(L1ProbeQuery), sizeof(L1ProbeQuery), queries.data());
        auto result_buffer = makeStorageBuffer(fixture.device(), queries.size() * 7u * sizeof(u32), sizeof(u32));

        rhi::ShaderCursor cursor{probe.m_root};
        bindBuffer(cursor, "g_l1_queries", query_buffer);
        bindBuffer(cursor, "g_l1_results", result_buffer);
        const std::array<u32, 4u> count{static_cast<u32>(queries.size()), 0u, 0u, 0u};
        bindConstant(cursor, "g_l1_probe", count);

        struct FineFrame {
            u32 m_frame_low;
            u32 m_frame_high;
            u32 m_fine_edge;
            u32 m_status_record;
            float m_current_weight;
            u32 m_first_field;
            u32 m_reserved0;
            u32 m_reserved1;
        };
        FineFrame fine{};
        fine.m_frame_low = static_cast<u32>(accepted_frame);
        fine.m_frame_high = static_cast<u32>(accepted_frame >> 32u);
        fine.m_fine_edge = kFineEdge;
        bindConstant(cursor, "g_fine", fine);

        dispatchProbe(fixture, probe, 1u);

        const auto words = readWords(fixture, result_buffer, 0u, queries.size() * 7u * sizeof(u32));
        std::vector<L1ProbeResult> results(queries.size());
        for (std::size_t index = 0u; index < results.size(); ++index) {
            const std::size_t base = index * 7u;
            results[index].m_phase = words[base + 0u];
            results[index].m_valid = words[base + 1u] != 0u;
            results[index].m_trace_low = words[base + 2u];
            results[index].m_trace_high = words[base + 3u];
            results[index].m_direction = float2{
                std::bit_cast<float>(words[base + 4u]), std::bit_cast<float>(words[base + 5u])};
            results[index].m_age = words[base + 6u];
        }

        return results;
    }

    [[nodiscard]] std::vector<float3> runAngularMeanProbe(
        Fixture &fixture, const std::vector<float4> &positions, rhi::IBuffer *const field) {
        PPR_TEST_ASSERT(not positions.empty());

        ProbeRunner probe = makeProbe(fixture, "probeAngularMean");
        auto position_buffer = makeShaderBuffer(
            fixture.device(), positions.size() * sizeof(float4), sizeof(float4), positions.data());
        auto result_buffer = makeStorageBuffer(fixture.device(), positions.size() * sizeof(float4), sizeof(float4));

        rhi::ShaderCursor cursor{probe.m_root};
        bindBuffer(cursor, "g_mean_positions", position_buffer);
        bindBuffer(cursor, "g_mean_results", result_buffer);
        bindBuffer(cursor, "g_fine_ch", field);
        const std::array<u32, 4u> count{static_cast<u32>(positions.size()), 0u, 0u, 0u};
        bindConstant(cursor, "g_mean_probe", count);

        dispatchProbe(fixture, probe, 1u);

        std::vector<float4> words(positions.size());
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().readBuffer(result_buffer, 0u, positions.size() * sizeof(float4), words.data())));
        std::vector<float3> results(positions.size());
        for (std::size_t index = 0u; index < results.size(); ++index) {
            results[index] = rgb(words[index].x, words[index].y, words[index].z);
        }

        return results;
    }

    // ------------------------------------------------------------------
    // field priming
    // ------------------------------------------------------------------

    // Copies one packed record into the owner's in-place field before an
    // attempt. Used to prove every byte is rewritten and that an invocation
    // reads only its own prior record.
    void primeFineRecord(
        Fixture &fixture, WorldLightingPass &pass, const u32 x, const u32 y, const lighting::PackedCH &record) {
        PPR_TEST_ASSERT(pass.fineCHForTest() != nullptr);
        auto staging = makeStorageBuffer(
            fixture.device(), sizeof(record), sizeof(lighting::PackedCH), record.m_words.data());

        auto encoder = makeEncoder(fixture);
        auto *const field = pass.fineCHForTest();
        encoder->setBufferState(staging, rhi::ResourceState::CopySource);
        encoder->setBufferState(field, rhi::ResourceState::CopyDestination);
        encoder->copyBuffer(
            field, static_cast<u64>(y * kFineEdge + x) * sizeof(lighting::PackedCH), staging, 0u,
            sizeof(lighting::PackedCH));
        submit(fixture, *encoder);
    }

    // ------------------------------------------------------------------
    // CH basis, fitting and field normalization
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (uniform_field_preserves_c0_normalization_and_zero_ac) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);

        const float3 emission = rgb(0.5f, 0.75f, 1.0f);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, uniformSources(emission));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Cold start: allocation happens on the first prepare, but an encoded
        // attempt that is never submitted leaves the zero-initialized field
        // untouched. The first accepted field then completely overwrites it.
        dropAttempt(fixture, pass, inputs.inputs(), 0u, 0.0, -1);
        {
            const auto zero = readWords(fixture, pass.fineCHForTest(), 0u, 64u);
            PPR_TEST_ASSERT(std::ranges::all_of(zero, [](const u32 word) { return word == 0u; }));
        }

        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);

        for (const std::array<u32, 2u> probe:
            {std::array<u32, 2u>{256u, 256u}, std::array<u32, 2u>{0u, 0u}, std::array<u32, 2u>{0u, 300u},
                std::array<u32, 2u>{300u, 0u}, std::array<u32, 2u>{511u, 511u}}) {
            const auto record = readFineRecord(fixture, pass, probe[0u], probe[1u]);
            const lighting::CH decoded = lighting::unpackCH(record);

            PPR_TEST_ASSERT(record.m_words[1u] == 0u);
            for (u32 word = 2u; word < 8u; ++word) {
                PPR_TEST_ASSERT(record.m_words[word] == 0u);
            }

            const float3 mean = decoded.m_coefficients[0] * static_cast<float>(kY0);
            const float3 expected_c0 = emission * static_cast<float>(1.0 / kY0);
            PPR_TEST_ASSERT(closeRGB9E5(decoded.m_coefficients[0], expected_c0));
            PPR_TEST_ASSERT(closeRGB9E5(mean, emission));
            PPR_TEST_ASSERT(std::abs(mean.x - decoded.m_coefficients[0].x * static_cast<float>(kY0)) <= 1.0e-6f);
            PPR_TEST_ASSERT(pass.fineCHForTest()->getDesc().elementSize == sizeof(lighting::PackedCH));
            PPR_TEST_ASSERT(pass.fineCHForTest()->getDesc().size == lighting::kFineCHBytes);
        }

        std::println("WORLD_LIGHTING_FINE uniform c0=y0_normalized ac=0 edges=clamped");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (fit_recovers_signed_rotated_harmonics) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        HOracle field;
        field.m_coefficients[0] = rgb(1.0f, 0.8f, 0.6f);
        field.m_coefficients[1] = rgb(0.10f, -0.05f, 0.02f); // cos1
        field.m_coefficients[2] = rgb(-0.03f, 0.04f, -0.06f); // sin1
        field.m_coefficients[3] = rgb(0.05f, 0.02f, -0.01f); // cos2
        field.m_coefficients[4] = rgb(0.01f, -0.02f, 0.03f); // sin2
        field.m_coefficients[5] = rgb(-0.04f, 0.03f, 0.02f); // cos3
        field.m_coefficients[6] = rgb(0.02f, -0.01f, 0.04f); // sin3
        field.m_coefficients[7] = rgb(0.03f, 0.01f, -0.02f); // cos4
        field.m_coefficients[8] = rgb(-0.01f, 0.02f, 0.01f); // sin4

        constexpr u32 kSamples = 64u;
        const double weight = 2.0 * std::numbers::pi / static_cast<double>(kSamples);
        const auto buildSamples = [&](const double rotation) {
            std::vector<CHProbeSample> samples(kSamples);
            for (u32 index = 0u; index < kSamples; ++index) {
                const double theta = 2.0 * std::numbers::pi * static_cast<double>(index) / kSamples + rotation;
                samples[index].m_radiance = field.evaluate(theta - rotation);
                samples[index].m_theta = static_cast<float>(theta);
                samples[index].m_weight = static_cast<float>(weight);
            }

            return samples;
        };

        const auto assertCoefficients = [&](const std::array<u32, 9u> &words, const std::array<float3, 9u> &expected) {
            lighting::PackedCH packed{};
            std::copy_n(words.begin(), 8u, packed.m_words.begin());
            const lighting::CH decoded = lighting::unpackCH(packed);
            const float scale = std::bit_cast<float>(packed.m_words[1u]);
            PPR_TEST_ASSERT(words[8u] == 0u);
            PPR_TEST_ASSERT(closeRGB9E5(decoded.m_coefficients[0], expected[0u]));

            const float tolerance = 0.51f * scale + 1.0e-6f;
            for (u32 coefficient = 1u; coefficient < 9u; ++coefficient) {
                const float3 actual = decoded.m_coefficients[coefficient];
                const float3 wanted = expected[coefficient];
                PPR_TEST_ASSERT(std::abs(actual.x - wanted.x) <= tolerance);
                PPR_TEST_ASSERT(std::abs(actual.y - wanted.y) <= tolerance);
                PPR_TEST_ASSERT(std::abs(actual.z - wanted.z) <= tolerance);
            }

            // Signed AC survives encode/decode; the field is not forced positive.
            PPR_TEST_ASSERT(scale > 0.0f);
            PPR_TEST_ASSERT(
                decoded.m_coefficients[1u].x < 0.0f or
                decoded.m_coefficients[2u].x < 0.0f or
                decoded.m_coefficients[2u].y < 0.0f or
                decoded.m_coefficients[1u].z > 0.0f);
        };

        {
            const auto samples = buildSamples(0.0);
            CHProbeCase probe{};
            probe.m_sample_count = kSamples;
            probe.m_first_field = 1u;
            const auto results = runCHProbe(fixture, {probe}, samples);
            assertCoefficients(results[0u], field.windowed());
        }

        {
            const double rotation = 0.37;
            const auto samples = buildSamples(rotation);
            CHProbeCase probe{};
            probe.m_sample_count = kSamples;
            probe.m_first_field = 1u;
            const auto results = runCHProbe(fixture, {probe}, samples);
            assertCoefficients(results[0u], [&] {
                auto rotated = rotatedCoefficients(field.m_coefficients, rotation);
                HOracle rotated_field{};
                rotated_field.m_coefficients = rotated;
                return rotated_field.windowed();
            }());
        }

        std::println("WORLD_LIGHTING_FINE harmonics signed=true rotated=true window_once=true");
    };

    PPR_UNIT_TEST (zero_weight_and_invalid_total_weight_are_not_promoted) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        constexpr u32 kSamples = 16u;
        const double weight = 2.0 * std::numbers::pi / static_cast<double>(kSamples);
        std::vector<CHProbeSample> base(kSamples);
        for (u32 index = 0u; index < kSamples; ++index) {
            const double theta = 2.0 * std::numbers::pi * static_cast<double>(index) / kSamples;
            base[index].m_radiance = rgb(
                1.0f + 0.2f * static_cast<float>(std::cos(theta)), 0.5f,
                0.25f + 0.1f * static_cast<float>(std::sin(2.0 * theta)));
            base[index].m_theta = static_cast<float>(theta);
            base[index].m_weight = static_cast<float>(weight);
        }

        auto with_zero = base;
        CHProbeSample ignored;
        ignored.m_radiance = rgb(1.0e6f, 1.0e6f, 1.0e6f);
        ignored.m_theta = 0.123f;
        ignored.m_weight = 0.0f;
        with_zero.push_back(ignored);

        CHProbeCase plain{};
        plain.m_sample_count = kSamples;
        plain.m_first_field = 1u;
        CHProbeCase zero_tap = plain;
        zero_tap.m_sample_count = kSamples + 1u;

        auto results = runCHProbe(fixture, {plain, zero_tap}, with_zero);
        PPR_TEST_ASSERT(results[0u] == results[1u]);
        PPR_TEST_ASSERT(results[0u][8u] == 0u);

        {
            CHProbeCase zero_total{};
            zero_total.m_sample_count = kSamples;
            zero_total.m_first_field = 1u;
            auto zero_samples = base;
            for (auto &sample: zero_samples) {
                sample.m_weight = 0.0f;
            }

            const auto failure = runCHProbe(fixture, {zero_total}, zero_samples);
            PPR_TEST_ASSERT(failure[0u][8u] == 1u);
            for (u32 word = 0u; word < 9u; ++word) {
                PPR_TEST_ASSERT(failure[0u][word] == (word == 8u ? 1u : 0u));
            }
        }

        {
            CHProbeCase invalid_total{};
            invalid_total.m_sample_count = kSamples;
            invalid_total.m_first_field = 1u;
            auto invalid_samples = base;
            invalid_samples[3u].m_weight = std::numeric_limits<float>::quiet_NaN();
            const auto failure = runCHProbe(fixture, {invalid_total}, invalid_samples);
            PPR_TEST_ASSERT(failure[0u][8u] == 1u);
        }

        std::println("WORLD_LIGHTING_FINE zero_weight skipped=true nonpositive_total=failure");
    };

    // ------------------------------------------------------------------
    // EMA, injected time and settlement
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (injected_active_time_matches_the_approved_ema) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);

        const float3 e0 = rgb(0.5f, 0.6f, 0.7f);
        const float3 e1 = rgb(2.0f, 1.5f, 1.0f);
        const float3 e2 = rgb(0.25f, 0.5f, 1.0f);
        const float3 e3 = rgb(3.0f, 2.0f, 1.0f);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // First accepted field is the fresh reconstruction, not a blend.
        inputs.setSources(fixture, uniformSources(e0));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(readFineRecord(fixture, pass, 100u, 100u)), e0));

        const double dt = 0.25;
        const float alpha = lighting::fineAccumulationWeight(dt);
        PPR_TEST_ASSERT(std::abs(alpha - static_cast<float>(1.0 - std::exp(-2.0 * dt))) <= 1.0e-6f);
        const float3 expected1 = e0 + (e1 - e0) * alpha;

        inputs.setSources(fixture, uniformSources(e1));
        runFrame(fixture, pass, inputs.inputs(), 1u, dt, -1, 2u);
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(readFineRecord(fixture, pass, 100u, 100u)), expected1));

        // Simulation pause does not pause accumulation: the injected active
        // time advances even though the scene inputs are unchanged.
        inputs.setSources(fixture, uniformSources(e3));
        runFrame(fixture, pass, inputs.inputs(), 2u, 1.0, -1, 3u);
        const float alpha_pause = lighting::fineAccumulationWeight(1.0 - dt);
        const float3 expected2 = expected1 + (e3 - expected1) * alpha_pause;
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(readFineRecord(fixture, pass, 100u, 100u)), expected2));

        // A suspended interval does not advance the injected active time: no
        // history replacement merely because wall-clock time passed.
        inputs.setSources(fixture, uniformSources(e2));
        runFrame(fixture, pass, inputs.inputs(), 3u, 1.0, -1, 4u);
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(readFineRecord(fixture, pass, 100u, 100u)), expected2));

        std::println("WORLD_LIGHTING_FINE ema alpha={} first=fresh pause=advances suspended=holds", alpha);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (rejected_attempts_preserve_phase_validity_and_timing) {
        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);

        const float3 e0 = rgb(0.5f, 0.6f, 0.7f);
        const float3 dropped = rgb(9.0f, 0.0f, 0.0f);
        const float3 final_emission = rgb(2.0f, 1.0f, 0.5f);

        // History A: an encoded-but-unsubmitted attempt at 0.5 s, then the
        // accepted frame at 1.0 s.
        // History B: no dropped attempt; the accepted frame is at 1.0 s.
        // Both must see the same 1.0 s active interval and the same phase.
        alignas(16) std::array<std::array<u32, 8u>, 2u> records{};
        for (u32 history = 0u; history < 2u; ++history) {
            Fixture fixture;
            PPR_DEFER {
                fixture.shutdown();
            };
            fixture.initialize();

            FineInputs inputs{visuals, map};
            inputs.uploadCommon(fixture);

            WorldLightingPass pass;
            PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

            inputs.setSources(fixture, uniformSources(e0));
            runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);

            if (history == 0u) {
                FineInputs dropped_inputs{visuals, map};
                dropped_inputs.uploadCommon(fixture);
                dropped_inputs.setSources(fixture, uniformSources(dropped));
                dropAttempt(fixture, pass, dropped_inputs.inputs(), 1u, 0.5, -1);

                PPR_TEST_ASSERT(pass.acceptedFrameForTest() == 1u);
                PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(readFineRecord(fixture, pass, 50u, 50u)), e0));
            }

            inputs.setSources(fixture, uniformSources(final_emission));
            runFrame(fixture, pass, inputs.inputs(), 1u, 1.0, -1, 2u);
            PPR_TEST_ASSERT(pass.acceptedFrameForTest() == 2u);

            const auto record = readFineRecord(fixture, pass, 50u, 50u);
            records[history] = record.m_words;
        }

        PPR_TEST_ASSERT(records[0u] == records[1u]);
        const float alpha = lighting::fineAccumulationWeight(1.0);
        const float3 expected = e0 + (final_emission - e0) * alpha;
        lighting::PackedCH packed{};
        packed.m_words = records[0u];
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(packed), expected));

        std::println("WORLD_LIGHTING_FINE rejection phase=frozen timing=carried validity=held");
    };

    // ------------------------------------------------------------------
    // connector geometry and source ordering
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (near_hit_with_uninitialized_far_interval_is_not_suppressed) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const float3 emission = rgb(4.0f, 2.0f, 1.0f);

        // A rectangular emitter covers the chosen fine probe. On the very first
        // accepted frame every L1 interval is still zero/never-initialized, so
        // this result can only come from the near connector tracing the current
        // cached source.
        CategoricalMap map;
        map.fillRect(200u, 200u, 219u, 219u, kMetal);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, uniformSources(emission));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);

        const auto inside = readFineRecord(fixture, pass, 100u, 100u);
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(inside), emission));
        PPR_TEST_ASSERT(inside.m_words[1u] == 0u);

        // Outside the emitter every near connector is clear and the
        // never-initialized far interval is (0,0), not vacuum sky.
        const auto outside = readFineRecord(fixture, pass, 400u, 400u);
        PPR_TEST_ASSERT(lighting::unpackCH(outside).m_coefficients[0].x == 0.0f);
        PPR_TEST_ASSERT(outside.m_words[1u] == 0u);

        std::println("WORLD_LIGHTING_FINE near_hit far_uninitialized=not_suppressing cold_far=zero");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (retained_l1_direction_identity_uses_the_probe_trace_frame) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const u64 accepted = 7u;
        std::vector<L1ProbeQuery> queries;
        for (u32 index = 0u; index < 16u; ++index) {
            queries.push_back({.m_probe_x = index * 13u, .m_probe_y = index * 7u, .m_phi = index, .m_reserved = 0u});
        }

        const auto results = runL1Probe(fixture, queries, accepted);
        u32 retained_directions = 0u;
        for (std::size_t index = 0u; index < queries.size(); ++index) {
            const auto &query = queries[index];
            const u32 phase = [&] {
                const u32 probe_edge = kEdge / 4u;
                const u32 group_edge = probe_edge / 2u;
                const u32 group_x = query.m_probe_x / 2u;
                const u32 group_y = query.m_probe_y / 2u;
                u32 value = group_x + group_edge * group_y + 0x9e3779b9u;
                value = (value ^ (value >> 16u)) * 0x7feb352du;
                value = (value ^ (value >> 15u)) * 0x846ca68bu;
                return (value ^ (value >> 16u)) & 3u;
            }();
            const u32 local = (query.m_probe_x % 2u) + 2u * (query.m_probe_y % 2u);
            const u32 age = ((static_cast<u32>(accepted) & 3u) + phase + 4u - local) & 3u;
            const bool valid = accepted >= age;
            const u64 trace = accepted - age;

            PPR_TEST_ASSERT(results[index].m_phase == phase);
            PPR_TEST_ASSERT(results[index].m_age == age);
            PPR_TEST_ASSERT(results[index].m_valid == valid);
            PPR_TEST_ASSERT(results[index].m_trace_low == static_cast<u32>(trace));
            PPR_TEST_ASSERT(results[index].m_trace_high == static_cast<u32>(trace >> 32u));

            const u32 direction_frame = valid ? static_cast<u32>(trace) : static_cast<u32>(accepted);
            const double fraction = (static_cast<double>(direction_frame * 0x9e3779b9u >> 9u) + 0.5) / 8'388'608.0;
            const double theta = 2.0 * std::numbers::pi * (static_cast<double>(query.m_phi) + fraction) / 16.0;
            PPR_TEST_ASSERT(std::abs(results[index].m_direction.x - static_cast<float>(std::cos(theta))) <= 1.0e-6f);
            PPR_TEST_ASSERT(std::abs(results[index].m_direction.y - static_cast<float>(std::sin(theta))) <= 1.0e-6f);

            if (valid and age != 0u) {
                ++retained_directions;
                // The retained probe's direction cannot be the current frame's
                // direction when the golden-ratio phase differs.
                const double current_fraction = (static_cast<double>(static_cast<u32>(accepted) * 0x9e3779b9u >> 9u) +
                                                    0.5) /
                                                8'388'608.0;
                const double current_theta = 2.0 *
                                             std::numbers::pi *
                                             (static_cast<double>(query.m_phi) + current_fraction) /
                                             16.0;
                PPR_TEST_ASSERT(
                    std::abs(results[index].m_direction.x - static_cast<float>(std::cos(current_theta))) > 0.0f or
                    std::abs(results[index].m_direction.y - static_cast<float>(std::sin(current_theta))) > 0.0f);
            }
        }

        PPR_TEST_ASSERT(retained_directions > 0u);
        std::println(
            "WORLD_LIGHTING_FINE retained_direction probes={} retained={} frame=current_vs_trace", queries.size(),
            retained_directions);
    };

    PPR_UNIT_TEST (fine_writes_all_bytes_and_reads_only_its_own_prior) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);

        const float3 e0 = rgb(0.5f, 0.6f, 0.7f);
        const float3 e1 = rgb(2.0f, 1.5f, 1.0f);
        inputs.setSources(fixture, uniformSources(e0));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Allocate the field with a definitely-unsubmitted attempt; the buffer
        // remains zero until a submitted frame executes.
        dropAttempt(fixture, pass, inputs.inputs(), 0u, 0.0, -1);

        // Poison the target record with a non-zero AC pattern. The first fine
        // field bypasses the blend, so every byte must be overwritten by the
        // cold-start reconstruction.
        lighting::PackedCH poison{};
        poison.m_words.fill(0x7f7f7f7fu);
        primeFineRecord(fixture, pass, 210u, 310u, poison);

        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);

        const auto first = readFineRecord(fixture, pass, 210u, 310u);
        PPR_TEST_ASSERT(first.m_words[0u] != poison.m_words[0u]);
        PPR_TEST_ASSERT(first.m_words[1u] == 0u);
        for (u32 word = 2u; word < 8u; ++word) {
            PPR_TEST_ASSERT(first.m_words[word] == 0u);
        }

        // A valid neighbour prior must not influence the target's EMA. The
        // target's own prior is the first field above.
        lighting::CH neighbour{};
        neighbour.m_coefficients[0] = rgb(10.0f, 10.0f, 10.0f);
        const auto neighbour_record = lighting::packCH(neighbour);
        PPR_TEST_ASSERT(neighbour_record.has_value());
        primeFineRecord(fixture, pass, 211u, 310u, *neighbour_record);

        const double dt = 0.5;
        inputs.setSources(fixture, uniformSources(e1));
        runFrame(fixture, pass, inputs.inputs(), 1u, dt, -1, 2u);

        const float alpha = lighting::fineAccumulationWeight(dt);
        const float3 expected_target = e0 + (e1 - e0) * alpha;
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(readFineRecord(fixture, pass, 210u, 310u)), expected_target));

        // The neighbour's own prior is the primed C0 = 10 coefficient, so its
        // decoded angular mean starts at 10*Y0 and blends toward e1.
        const float3 neighbour_prior = rgb(10.0f, 10.0f, 10.0f) * static_cast<float>(kY0);
        const float3 expected_neighbour = neighbour_prior + (e1 - neighbour_prior) * alpha;
        PPR_TEST_ASSERT(closeRGB9E5(fineAngularMean(readFineRecord(fixture, pass, 211u, 310u)), expected_neighbour));

        // One in-place allocation, never replaced per frame.
        PPR_TEST_ASSERT(pass.fineCHForTest() != nullptr);
        PPR_TEST_ASSERT(pass.fineCHForTest()->getDesc().size == lighting::kFineCHBytes);
        rhi::IBuffer *const stable = pass.fineCHForTest();
        runFrame(fixture, pass, inputs.inputs(), 2u, dt + 0.1, -1, 3u);
        PPR_TEST_ASSERT(pass.fineCHForTest() == stable);

        std::println("WORLD_LIGHTING_FINE in_place all_32_bytes=true own_prior_only=true single_allocation=true");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (source_reads_precede_fine_writes_across_successive_frames) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kDiffuse);
        const float3 emission = rgb(0.5f, 0.5f, 0.5f);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);

        const auto diffuse_sources = [&] {
            std::array<lighting::SourceMaterial, kMaterialCount> sources{};
            const auto record = lighting::makeSourceMaterial(
                emission, 1.0, rgb(1.0f, 1.0f, 1.0f), rgb(1.0f, 1.0f, 1.0f), 0.0);
            PPR_TEST_ASSERT(record.has_value());
            for (auto &source: sources) {
                source = *record;
            }

            return sources;
        }();

        inputs.setSources(fixture, diffuse_sources);
        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);
        const auto cold_source = lighting::unpackRGB9E5(
            readWords(fixture, pass.sourceCellsForTest(), cellIndex(512u, 512u) * 4u, 4u)[0u]);
        PPR_TEST_ASSERT(closeRGB9E5(cold_source, emission));

        const float3 field_mean = fineAngularMean(readFineRecord(fixture, pass, 256u, 256u));
        PPR_TEST_ASSERT(closeRGB9E5(field_mean, emission));

        // Frame 1's source must read the frame-0 field, not the field that this
        // submission is about to write. With unit diffuse the expected source is
        // emission + emission = 2*emission; reading the new field would give 3x.
        runFrame(fixture, pass, inputs.inputs(), 1u, 0.25, -1, 2u);
        const auto warm_source = lighting::unpackRGB9E5(
            readWords(fixture, pass.sourceCellsForTest(), cellIndex(512u, 512u) * 4u, 4u)[0u]);
        PPR_TEST_ASSERT(closeRGB9E5(warm_source, emission * 2.0f));
        PPR_TEST_ASSERT(not closeRGB9E5(warm_source, emission * 3.0f));

        std::println("WORLD_LIGHTING_FINE barriers source_pre_update=true fine_then_next_source=true");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (fine_range_failure_reports_status_and_containment) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);

        // RGB9E5 source radiance up to 65,408 is representable, but its C0 =
        // mean/Y0 exceeds the DC representable limit, so fine reconstruction
        // must report a range failure rather than clamp or renormalize.
        const float3 emission = rgb(60'000.0f, 0.0f, 0.0f);
        inputs.setSources(fixture, uniformSources(emission));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        submitAttempt(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);
        {
            const auto words = pass.pendingStatusWordsForTest();
            PPR_TEST_ASSERT(words.size() == kNumericStatusRecords * kNumericStatusWords);
            PPR_TEST_ASSERT(words[kNumericStageFine * kNumericStatusWords + 0u] == kNoNumericIndex);
            PPR_TEST_ASSERT(words[kNumericStageFine * kNumericStatusWords + 1u] == 0u);
            for (u32 word = 2u; word < kNumericStatusWords; ++word) {
                PPR_TEST_ASSERT(words[kNumericStageFine * kNumericStatusWords + word] == 0u);
            }
        }

        PPR_TEST_ASSERT(pass.collectCompleted(1u) == std::errc::result_out_of_range);
        const auto contained = readFineRecord(fixture, pass, 0u, 0u);
        for (const u32 word: contained.m_words) {
            PPR_TEST_ASSERT(word == 0u);
        }

        std::println("WORLD_LIGHTING_FINE status range=7 index=0 containment=zero");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (angular_mean_helper_matches_bilinear_oracle) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        std::vector<lighting::PackedCH> field(lighting::kFineCHRecordCount);
        for (u32 y = 0u; y < kFineEdge; ++y) {
            for (u32 x = 0u; x < kFineEdge; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(kFineEdge - 1u);
                const float v = static_cast<float>(y) / static_cast<float>(kFineEdge - 1u);
                const auto dc = lighting::packRGB9E5(rgb(0.1f + 0.5f * u, 0.2f + 0.3f * v, 0.4f));
                PPR_TEST_ASSERT(dc.has_value());
                field[static_cast<std::size_t>(y) * kFineEdge + x].m_words[0u] = *dc;
            }
        }

        auto field_buffer = makeStorageBuffer(
            fixture.device(), field.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH), field.data());

        const std::vector<float4> positions{
            float4{0.5f, 0.5f, 0.0f, 0.0f}, float4{512.5f, 512.5f, 0.0f, 0.0f}, float4{1023.5f, 1023.5f, 0.0f, 0.0f},
            float4{0.5f, 700.25f, 0.0f, 0.0f}, float4{733.25f, 0.5f, 0.0f, 0.0f}};

        const auto actual = runAngularMeanProbe(fixture, positions, field_buffer);

        for (std::size_t index = 0u; index < positions.size(); ++index) {
            const double fine_x = static_cast<double>(positions[index].x) * 0.5 - 0.5;
            const double fine_y = static_cast<double>(positions[index].y) * 0.5 - 0.5;
            const i32 base_x = static_cast<i32>(std::floor(fine_x));
            const i32 base_y = static_cast<i32>(std::floor(fine_y));
            const double fraction_x = fine_x - base_x;
            const double fraction_y = fine_y - base_y;

            double3 mean{0.0, 0.0, 0.0};
            for (u32 tap = 0u; tap < 4u; ++tap) {
                const i32 offset_x = static_cast<i32>(tap % 2u);
                const i32 offset_y = static_cast<i32>(tap / 2u);
                const double weight = (offset_x == 0 ? 1.0 - fraction_x : fraction_x) *
                                      (offset_y == 0 ? 1.0 - fraction_y : fraction_y);
                if (weight <= 0.0) {
                    continue;
                }

                const u32 sample_x = static_cast<u32>((std::clamp) (base_x + offset_x, 0, 511));
                const u32 sample_y = static_cast<u32>((std::clamp) (base_y + offset_y, 0, 511));
                const float3 dc = lighting::unpackRGB9E5(*lighting::packRGB9E5(
                    rgb(0.1f + 0.5f * static_cast<float>(sample_x) / 511.0f,
                        0.2f + 0.3f * static_cast<float>(sample_y) / 511.0f, 0.4f)));
                mean.x += static_cast<double>(dc.x) * weight;
                mean.y += static_cast<double>(dc.y) * weight;
                mean.z += static_cast<double>(dc.z) * weight;
            }

            const float3 expected{
                static_cast<float>(mean.x * kY0), static_cast<float>(mean.y * kY0), static_cast<float>(mean.z * kY0)};
            PPR_TEST_ASSERT(closeRGB9E5(actual[index], expected, 2.0e-3f));
        }

        std::println("WORLD_LIGHTING_FINE angular_mean y0_normalized=true extra_factors=none");
    };

    // ------------------------------------------------------------------
    // reflected ABI
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (reflected_fine_bindings_match_the_cpu_shapes) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "world_lighting_fine.slang", "world_lighting_fine",
            module.writeRef()));

        const auto field_stride = [&](const std::string_view name) -> u32 {
            auto *const layout = module->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (name == std::string_view{parameter->getName()}) {
                    return safe_narrowing(parameter->getTypeLayout()->getElementTypeLayout()->getStride(
                        SLANG_PARAMETER_CATEGORY_UNIFORM));
                }
            }

            return 0u;
        };

        PPR_TEST_ASSERT(field_stride("g_fine_ch") == 32u);
        PPR_TEST_ASSERT(field_stride("g_cascades") == 8u);
        PPR_TEST_ASSERT(field_stride("g_resolved_cells") == 4u);
        PPR_TEST_ASSERT(field_stride("g_cell_source") == 4u);

        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        slang::TypeLayoutReflection *fine_constants = nullptr;
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_fine") {
                fine_constants = parameter->getTypeLayout()->getElementTypeLayout();
            }
        }

        PPR_TEST_ASSERT(fine_constants != nullptr);
        PPR_TEST_ASSERT(fine_constants->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u);
        PPR_TEST_ASSERT(fine_constants->getFieldByIndex(0u)->getOffset() == 0u);
        PPR_TEST_ASSERT(fine_constants->getFieldByIndex(1u)->getOffset() == 4u);
        PPR_TEST_ASSERT(fine_constants->getFieldByIndex(2u)->getOffset() == 8u);
        PPR_TEST_ASSERT(fine_constants->getFieldByIndex(3u)->getOffset() == 12u);
        PPR_TEST_ASSERT(fine_constants->getFieldByIndex(4u)->getOffset() == 16u);
        PPR_TEST_ASSERT(fine_constants->getFieldByIndex(5u)->getOffset() == 20u);

        shader::ComPtr<slang::IEntryPoint> point{};
        PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("reconstructFine", point.writeRef())));

        PPR_TEST_ASSERT(WorldLightingPass::kFineCHEdge == 512u);
        PPR_TEST_ASSERT(WorldLightingPass::kFineCHRecordCount == 262'144u);
        PPR_TEST_ASSERT(WorldLightingPass::kFineCHBytes == 8'388'608u);
        PPR_TEST_ASSERT(WorldLightingPass::kFineDispatchThreads == 8u);

        std::println("WORLD_LIGHTING_FINE reflected ch_stride=32 cascade_stride=8 constants=32 entry=reconstructFine");
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // fine CH reconstruction and in-place accumulation suite
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_fineTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_fine") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingFine::uniform_field_preserves_c0_normalization_and_zero_ac,
                detail::WorldLightingFine::fit_recovers_signed_rotated_harmonics,
                detail::WorldLightingFine::zero_weight_and_invalid_total_weight_are_not_promoted,
                detail::WorldLightingFine::injected_active_time_matches_the_approved_ema,
                detail::WorldLightingFine::rejected_attempts_preserve_phase_validity_and_timing,
                detail::WorldLightingFine::near_hit_with_uninitialized_far_interval_is_not_suppressed,
                detail::WorldLightingFine::retained_l1_direction_identity_uses_the_probe_trace_frame,
                detail::WorldLightingFine::fine_writes_all_bytes_and_reads_only_its_own_prior,
                detail::WorldLightingFine::source_reads_precede_fine_writes_across_successive_frames,
                detail::WorldLightingFine::fine_range_failure_reports_status_and_containment,
                detail::WorldLightingFine::angular_mean_helper_matches_bilinear_oracle,
                detail::WorldLightingFine::reflected_fine_bindings_match_the_cpu_shapes,
            });
        };
        return group;
    }
}
