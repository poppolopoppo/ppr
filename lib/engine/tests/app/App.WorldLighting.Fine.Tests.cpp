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
    inline constexpr u32 kMaterialCount = 9u;

    inline constexpr u32 kMetal = 1u;
    inline constexpr u32 kDiffuse = 2u;
    inline constexpr u32 kOpaque = 3u;
    inline constexpr u32 kBack = 4u;
    inline constexpr u32 kRed = 5u;
    inline constexpr u32 kGreen = 6u;
    inline constexpr u32 kBlue = 7u;
    inline constexpr u32 kYellow = 8u;

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

        void initialize(UnitTest::IRun &_) {
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
            _.log(
                std::format(
                    "WORLD_LIGHTING_FINE api={} adapter={} backend={}",
                    info.apiName != nullptr ? info.apiName : "unknown",
                    info.adapterName != nullptr ? info.adapterName : "unknown", static_cast<int>(info.deviceType)));
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

        void clearRect(const u32 x0, const u32 y0, const u32 x1, const u32 y1) {
            for (u32 y = y0; y <= y1; ++y) {
                for (u32 x = x0; x <= x1; ++x) {
                    m_words[cellIndex(x, y)] = 0u;
                }
            }
        }

        // The probe's own/neighbour 2x2 native block, opened to vacuum so its
        // near trace sees the surrounding room through an ordinary hit.
        void clearProbeBlock(const u32 probe_x, const u32 probe_y) {
            clearRect(probe_x * 2u, probe_y * 2u, probe_x * 2u + 1u, probe_y * 2u + 1u);
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
        rhi::ComPtr<rhi::IBuffer> m_prior_ch{};

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

        // A supplied uniform DC field, used only by the interior-tap filter test.
        void setUniformPriorCH(Fixture &fixture, const float3 value) {
            const auto word = lighting::packRGB9E5(value);
            PPR_TEST_ASSERT(word.has_value());
            std::vector<lighting::PackedCH> records(lighting::kFineCHRecordCount);
            for (auto &record: records) {
                record.m_words.fill(0u);
                record.m_words[0u] = *word;
            }

            m_prior_ch = makeShaderBuffer(
                fixture.device(), lighting::kFineCHBytes, sizeof(lighting::PackedCH), records.data());
        }

        [[nodiscard]] lighting::WorldLightingInputs inputs() const {
            lighting::WorldLightingInputs result{};
            result.m_terrain = m_terrain;
            result.m_visible_materials = m_palette;
            result.m_source_materials = m_sources;
            result.m_thermal_lut = m_thermal;
            result.m_prior_ch = m_prior_ch;
            result.m_material_count = kMaterialCount;
            return result;
        }
    };

    // ------------------------------------------------------------------
    // nonzero-AC quadrant fixture
    // ------------------------------------------------------------------

    // Four metallic materials with a known emission per quadrant around the
    // chosen fine centre. The colored cells form the twelve-cell ring just
    // outside the probe's own 2x2 block, and the block itself is vacuum: the
    // near trace sees the ring through an ordinary hit, so the angular
    // distribution still comes only from the connector basis.
    [[nodiscard]] std::array<lighting::SourceMaterial, kMaterialCount> makeQuadrantSources() {
        std::array<lighting::SourceMaterial, kMaterialCount> sources{};
        const auto store = [&](const u32 material, const float3 emission) {
            const auto record = lighting::makeSourceMaterial(
                emission, 1.0, rgb(1.0f, 1.0f, 1.0f), rgb(1.0f, 1.0f, 1.0f), 1.0);
            PPR_TEST_ASSERT(record.has_value());
            sources[material] = *record;
        };

        store(kRed, rgb(4.0f, 0.0f, 0.0f));
        store(kGreen, rgb(0.0f, 4.0f, 0.0f));
        store(kBlue, rgb(0.0f, 0.0f, 4.0f));
        store(kYellow, rgb(2.0f, 2.0f, 0.0f));
        return sources;
    }

    struct QuadrantFixture {
        // Fine probe (100,100) has centre (201,201), the shared corner of its
        // own 2x2 block. The block is vacuum; the colored quadrants occupy the
        // ring around it, assigned by the side of the corner the cell centre
        // falls on, which reproduces the block's directional partition.
        CategoricalMap m_map;
        std::array<lighting::SourceMaterial, kMaterialCount> m_sources;
        std::vector<u32> m_source_words;
        std::vector<u8> m_occupancy;

        QuadrantFixture() {
            constexpr u32 kCornerX = 201u;
            constexpr u32 kCornerY = 201u;

            m_map.fill(kBack);
            m_map.clearProbeBlock(100u, 100u);
            m_sources = makeQuadrantSources();
            m_source_words.assign(kCells, 0u);
            for (u32 y = kCornerY - 2u; y <= kCornerY + 1u; ++y) {
                for (u32 x = kCornerX - 2u; x <= kCornerX + 1u; ++x) {
                    const bool own_block = x >= kCornerX - 1u and
                                           x <= kCornerX and
                                           y >= kCornerY - 1u and
                                           y <= kCornerY;
                    if (own_block) {
                        continue;
                    }

                    const bool east = static_cast<float>(x) + 0.5f > static_cast<float>(kCornerX);
                    const bool north = static_cast<float>(y) + 0.5f > static_cast<float>(kCornerY);
                    const u32 material = east ? (north ? kYellow : kGreen) : (north ? kBlue : kRed);
                    m_map.occupy(x, y, material);
                    m_source_words[cellIndex(x, y)] = m_sources[material].m_emission;
                }
            }

            m_occupancy.assign(kCells, 1u);
            for (u32 y = kCornerY - 1u; y <= kCornerY; ++y) {
                for (u32 x = kCornerX - 1u; x <= kCornerX; ++x) {
                    m_occupancy[cellIndex(x, y)] = 0u;
                }
            }
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
    // independent fine connector geometry oracle
    // ------------------------------------------------------------------

    inline constexpr float kTraceFarOracle = 3.402823466e+38f;

    [[nodiscard]] u32 fineMix32(u32 value) noexcept {
        value = (value ^ (value >> 16u)) * 0x7feb352du;
        value = (value ^ (value >> 15u)) * 0x846ca68bu;
        return value ^ (value >> 16u);
    }

    // Level 1 group edge is 128 and the level constant is 1.
    [[nodiscard]] u32 fineL1Phase(const u32 probe_x, const u32 probe_y) noexcept {
        return fineMix32(probe_x / 2u + 128u * (probe_y / 2u) + 0x9e3779b9u) & 3u;
    }

    struct L1OracleFrame {
        bool m_valid{};
        u32 m_trace{};
        u32 m_age{};
    };

    [[nodiscard]] L1OracleFrame fineL1Frame(const u64 accepted, const u32 probe_x, const u32 probe_y) noexcept {
        const u32 phase = fineL1Phase(probe_x, probe_y);
        const u32 local = (probe_x % 2u) + 2u * (probe_y % 2u);
        const u32 age = ((static_cast<u32>(accepted) & 3u) + phase + 4u - local) & 3u;
        const bool valid = accepted >= age;
        return L1OracleFrame{
            .m_valid = valid,
            .m_trace = valid ? static_cast<u32>(accepted - age) : static_cast<u32>(accepted),
            .m_age = age};
    }

    [[nodiscard]] double finePhaseFraction(const u32 trace) noexcept {
        const u32 word = trace * 0x9e3779b9u;
        return (static_cast<double>(word >> 9u) + 0.5) / 8'388'608.0;
    }

    [[nodiscard]] double fineConnectorAngle(const u32 phi, const u32 trace) noexcept {
        return 2.0 * std::numbers::pi * (static_cast<double>(phi) + finePhaseFraction(trace)) / 16.0;
    }

    struct FineTap {
        u32 m_x{};
        u32 m_y{};
        float m_weight{};
    };

    [[nodiscard]] std::array<FineTap, 4u> fineTaps(const u32 probe_x, const u32 probe_y) noexcept {
        const float2 fine_center{
            (static_cast<float>(probe_x) + 0.5f) * 2.0f, (static_cast<float>(probe_y) + 0.5f) * 2.0f};
        const float2 relative{fine_center.x * 0.25f - 0.5f, fine_center.y * 0.25f - 0.5f};
        const i32 base_x = static_cast<i32>(std::floor(relative.x));
        const i32 base_y = static_cast<i32>(std::floor(relative.y));
        const float fraction_x = relative.x - static_cast<float>(base_x);
        const float fraction_y = relative.y - static_cast<float>(base_y);
        const std::array<i32, 4u> offset_x{0, 1, 0, 1};
        const std::array<i32, 4u> offset_y{0, 0, 1, 1};

        std::array<FineTap, 4u> taps{};
        for (u32 index = 0u; index < 4u; ++index) {
            taps[index].m_x = static_cast<u32>((std::clamp) (base_x + offset_x[index], 0, 255));
            taps[index].m_y = static_cast<u32>((std::clamp) (base_y + offset_y[index], 0, 255));
            taps[index].m_weight = (offset_x[index] == 0 ? 1.0f - fraction_x : fraction_x) *
                                   (offset_y[index] == 0 ? 1.0f - fraction_y : fraction_y);
        }

        return taps;
    }

    struct FineOracleInputs {
        const std::vector<u8> &m_occupancy;
        const std::vector<u32> &m_source;
        const std::vector<u32> *m_l1{};
        u32 m_edge{};
    };

    [[nodiscard]] lighting::Interval fineFarInterval(
        const FineOracleInputs &inputs, const u32 probe_x, const u32 probe_y, const u32 phi, const bool valid) {
        lighting::Interval result{};
        if (not valid or inputs.m_l1 == nullptr) {
            return result;
        }

        constexpr u32 spacing = 4u;
        const u32 atlas_x = probe_x * spacing + phi % spacing;
        const u32 atlas_y = probe_y * spacing + phi / spacing;
        const std::size_t record = static_cast<std::size_t>(atlas_y) * 1024u + atlas_x;
        lighting::PackedInterval packed{};
        packed.m_rg = (*inputs.m_l1)[record * 2u];
        packed.m_bt = (*inputs.m_l1)[record * 2u + 1u];
        return lighting::unpackInterval(packed);
    }

    [[nodiscard]] double fineChannel(const float3 value, const u32 channel) noexcept {
        return static_cast<double>(channel == 0u ? value.x : (channel == 1u ? value.y : value.z));
    }

    // Replays all 64 nominal connectors with independent geometry, traversal,
    // far-interval and spec-formula fitting. Inputs are the CPU-known resolved
    // occupancy/source plus the GPU L1 store the fine dispatch read.
    [[nodiscard]] std::array<float3, 9u> fineConnectorOracle(
        const u32 probe_x, const u32 probe_y, const u64 accepted, const FineOracleInputs &inputs) {
        const float2 fine_center{
            (static_cast<float>(probe_x) + 0.5f) * 2.0f, (static_cast<float>(probe_y) + 0.5f) * 2.0f};
        const float2 box_min{fine_center.x - 4.0f, fine_center.y - 4.0f};
        const float2 box_max{fine_center.x + 4.0f, fine_center.y + 4.0f};

        // An occupied cell inside the probe's own 2x2 block is an opaque black
        // self-occluder: the near ray terminates with zero radiance and zero
        // transmittance. The shared DDA returns the first occupied cell, so the
        // oracle maps an own-block hit to the black interval and every other
        // hit to its cached source. Vacuum block cells never become a DDA hit
        // and stay transparent, exactly like the shader.
        const u32 block_min_x = probe_x * 2u;
        const u32 block_min_y = probe_y * 2u;
        const auto own_block = [&](const u32 cell) {
            const u32 x = cell % inputs.m_edge;
            const u32 y = cell / inputs.m_edge;
            return x >= block_min_x and x <= block_min_x + 1u and y >= block_min_y and y <= block_min_y + 1u;
        };

        const std::span<const u8> occupancy{inputs.m_occupancy.data(), inputs.m_occupancy.size()};
        const auto taps = fineTaps(probe_x, probe_y);
        constexpr double direction_weight = 2.0 * std::numbers::pi / 16.0;

        struct Sample {
            float3 m_radiance;
            double m_theta;
            double m_weight;
        };

        std::vector<Sample> samples;
        samples.reserve(64u);
        for (const FineTap &tap: taps) {
            if (tap.m_weight <= 0.0f) {
                continue;
            }

            const L1OracleFrame frame = fineL1Frame(accepted, tap.m_x, tap.m_y);
            const float2 center{
                (static_cast<float>(tap.m_x) + 0.5f) * 4.0f, (static_cast<float>(tap.m_y) + 0.5f) * 4.0f};

            for (u32 phi = 0u; phi < 16u; ++phi) {
                const double angle = fineConnectorAngle(phi, frame.m_trace);
                const float2 direction{static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};

                float slip_x = kTraceFarOracle;
                float slip_y = kTraceFarOracle;
                if (direction.x > 0.0f) {
                    slip_x = (box_max.x - center.x) / direction.x;
                } else if (direction.x < 0.0f) {
                    slip_x = (box_min.x - center.x) / direction.x;
                }
                if (direction.y > 0.0f) {
                    slip_y = (box_max.y - center.y) / direction.y;
                } else if (direction.y < 0.0f) {
                    slip_y = (box_min.y - center.y) / direction.y;
                }

                const float exit_parameter = std::min(slip_x, slip_y);
                const float2 exit_point{
                    center.x + direction.x * exit_parameter, center.y + direction.y * exit_parameter};
                const float2 connector{exit_point.x - fine_center.x, exit_point.y - fine_center.y};
                const float connector_length = std::sqrt(connector.x * connector.x + connector.y * connector.y);
                const float2 basis = connector_length == 0.0f
                                         ? direction
                                         : float2{connector.x / connector_length, connector.y / connector_length};

                const float2 start{fine_center.x + basis.x * 0.001f, fine_center.y + basis.y * 0.001f};
                const std::optional<u32> hit = lighting::traceNativeCellDDA(
                    occupancy, inputs.m_edge, start, exit_point);

                lighting::Interval near_interval{};
                near_interval.m_transmittance = 1.0f;
                if (hit.has_value()) {
                    near_interval.m_radiance = own_block(*hit) ? float3{zero_v}
                                                               : lighting::unpackRGB9E5(inputs.m_source[*hit]);
                    near_interval.m_transmittance = 0.0f;
                }

                const lighting::Interval far_interval = fineFarInterval(inputs, tap.m_x, tap.m_y, phi, frame.m_valid);
                const float3 radiance = near_interval.m_radiance +
                                        far_interval.m_radiance * near_interval.m_transmittance;
                const double theta = std::atan2(static_cast<double>(basis.y), static_cast<double>(basis.x));
                samples.push_back({radiance, theta, static_cast<double>(tap.m_weight) * direction_weight});
            }
        }

        double total_weight = 0.0;
        std::array<double, 3u> weighted{};
        for (const Sample &sample: samples) {
            total_weight += sample.m_weight;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                weighted[channel] += fineChannel(sample.m_radiance, channel) * sample.m_weight;
            }
        }

        PPR_TEST_ASSERT(total_weight > 0.0);
        std::array<double, 3u> mean{};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            mean[channel] = weighted[channel] / total_weight;
        }

        std::array<float3, 9u> result;
        result[0u] = float3{
            static_cast<float>(mean[0u] / kY0), static_cast<float>(mean[1u] / kY0), static_cast<float>(mean[2u] / kY0)};
        for (u32 m = 1u; m <= 4u; ++m) {
            std::array<double, 3u> cc{};
            std::array<double, 3u> cs{};
            for (const Sample &sample: samples) {
                const double cosine = std::cos(static_cast<double>(m) * sample.m_theta);
                const double sine = std::sin(static_cast<double>(m) * sample.m_theta);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const double deviation = fineChannel(sample.m_radiance, channel) - mean[channel];
                    cc[channel] += deviation * (sample.m_weight * cosine);
                    cs[channel] += deviation * (sample.m_weight * sine);
                }
            }

            const double factor = (2.0 * std::numbers::pi / total_weight) * lanczos(m) / std::sqrt(std::numbers::pi);
            result[2u * m - 1u] = float3{
                static_cast<float>(cc[0u] * factor), static_cast<float>(cc[1u] * factor),
                static_cast<float>(cc[2u] * factor)};
            result[2u * m] = float3{
                static_cast<float>(cs[0u] * factor), static_cast<float>(cs[1u] * factor),
                static_cast<float>(cs[2u] * factor)};
        }

        return result;
    }

    void assertFineMatchesOracle(const lighting::PackedCH &record, const std::array<float3, 9u> &expected) {
        const lighting::CH decoded = lighting::unpackCH(record);
        PPR_TEST_ASSERT(closeRGB9E5(decoded.m_coefficients[0u], expected[0u], 5.0e-3f));
        const float scale = std::bit_cast<float>(record.m_words[1u]);
        const float tolerance = 0.55f * scale + 2.0e-4f;
        for (u32 coefficient = 1u; coefficient < 9u; ++coefficient) {
            const float3 actual = decoded.m_coefficients[coefficient];
            const float3 wanted = expected[coefficient];
            PPR_TEST_ASSERT(std::abs(actual.x - wanted.x) <= tolerance);
            PPR_TEST_ASSERT(std::abs(actual.y - wanted.y) <= tolerance);
            PPR_TEST_ASSERT(std::abs(actual.z - wanted.z) <= tolerance);
        }
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
        auto field_buffer = makeStorageBuffer(
            fixture.device(), cases.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH));

        rhi::ShaderCursor cursor{probe.m_root};
        bindBuffer(cursor, "g_ch_cases", case_buffer);
        bindBuffer(cursor, "g_ch_samples", sample_buffer);
        bindBuffer(cursor, "g_ch_results", result_buffer);
        bindOptionalBuffer(cursor, "g_fine_ch", field_buffer);
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
        fixture.initialize(_);

        const auto visuals = makePalette();

        // A uniform metallic emitter with each tested probe's own 2x2 block
        // opened to vacuum. The probe's own surface is then transparent (no
        // occupied block cell terminates it) and every near direction reaches
        // the uniform ring, which is the field this test normalises.
        constexpr std::array<std::array<u32, 2u>, 5u> kProbes{
            std::array<u32, 2u>{256u, 256u}, std::array<u32, 2u>{2u, 2u}, std::array<u32, 2u>{2u, 300u},
            std::array<u32, 2u>{300u, 2u}, std::array<u32, 2u>{509u, 509u}};
        CategoricalMap map;
        map.fill(kMetal);
        for (const auto &probe: kProbes) {
            map.clearProbeBlock(probe[0u], probe[1u]);
        }

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

        for (const auto &probe: kProbes) {
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
            PPR_TEST_ASSERT(pass.fineCHForTest()->getDesc().elementSize == sizeof(lighting::PackedCH));
            PPR_TEST_ASSERT(pass.fineCHForTest()->getDesc().size == lighting::kFineCHBytes);
        }

        // The outermost solid probes have no open block: their own block is a
        // black self-occluder, so the domain-edge field is the uniform zero,
        // still C0-only with zero AC. This is the edge uniformity contract.
        for (const std::array<u32, 2u> probe: {std::array<u32, 2u>{0u, 0u}, std::array<u32, 2u>{511u, 511u}}) {
            const auto record = readFineRecord(fixture, pass, probe[0u], probe[1u]);
            for (const u32 word: record.m_words) {
                PPR_TEST_ASSERT(word == 0u);
            }
        }

        _.log(std::format("WORLD_LIGHTING_FINE uniform c0=y0_normalized ac=0 edges=self_occluded_zero"));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (fit_recovers_signed_rotated_harmonics) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

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

        _.log(std::format("WORLD_LIGHTING_FINE harmonics signed=true rotated=true window_once=true"));
    };

    PPR_UNIT_TEST (zero_weight_and_invalid_total_weight_are_not_promoted) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

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

        _.log(std::format("WORLD_LIGHTING_FINE zero_weight skipped=true nonpositive_total=failure"));
    };

    // ------------------------------------------------------------------
    // EMA, injected time and settlement
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (injected_active_time_matches_the_approved_ema) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        // The probe's own block is opened so the near trace reaches the uniform
        // emitter ring instead of its black self-occluder.
        map.clearProbeBlock(100u, 100u);
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

        _.log(std::format("WORLD_LIGHTING_FINE ema alpha={} first=fresh pause=advances suspended=holds", alpha));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (rejected_attempts_preserve_phase_validity_and_timing) {
        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        // Probe (50,50) reads a uniform emitter ring through an open block.
        map.clearProbeBlock(50u, 50u);

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
            fixture.initialize(_);

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

        _.log(std::format("WORLD_LIGHTING_FINE rejection phase=frozen timing=carried validity=held"));
    };

    // ------------------------------------------------------------------
    // connector geometry and source ordering
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (near_hit_with_uninitialized_far_interval_is_not_suppressed) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        const float3 emission = rgb(4.0f, 2.0f, 1.0f);

        // A rectangular emitter covers the chosen fine probe and the ring
        // outside the probe's own 2x2 block, which is opened to vacuum so the
        // near trace sees the emitter through an ordinary hit instead of its
        // own black self-occluder. On the very first accepted frame every L1
        // interval is still zero/never-initialized, so this result can only come
        // from the near connector tracing the current cached source.
        CategoricalMap map;
        map.fillRect(196u, 196u, 219u, 219u, kMetal);
        map.clearProbeBlock(100u, 100u);
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

        _.log(std::format("WORLD_LIGHTING_FINE near_hit far_uninitialized=not_suppressing cold_far=zero"));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // real reconstructFine geometry, far merge and window-once EMA
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (connector_geometry_produces_signed_harmonics) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        QuadrantFixture quadrant;
        FineInputs inputs{visuals, quadrant.m_map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, quadrant.m_sources);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);

        const auto record = readFineRecord(fixture, pass, 100u, 100u);
        const auto l1 = readWords(fixture, pass.cascadeIntervalsForTest(), 0u, lighting::kCascadeLevelBytes);
        const FineOracleInputs oracle_inputs{quadrant.m_occupancy, quadrant.m_source_words, &l1, kEdge};
        const auto expected = fineConnectorOracle(100u, 100u, 0u, oracle_inputs);
        assertFineMatchesOracle(record, expected);

        // The four source quadrants around the fine centre produce a signed
        // first harmonic. The orientation is fixed by the red/green/blue/yellow
        // cell assignment, not by a uniform source.
        PPR_TEST_ASSERT(record.m_words[1u] != 0u);
        PPR_TEST_ASSERT(expected[1u].x < -1.0e-4f and expected[1u].y > 1.0e-4f and expected[1u].z < -1.0e-4f);
        PPR_TEST_ASSERT(expected[2u].x < -1.0e-4f and expected[2u].y < -1.0e-4f and expected[2u].z > 1.0e-4f);

        const lighting::CH decoded = lighting::unpackCH(record);
        const float scale = std::bit_cast<float>(record.m_words[1u]);
        _.log(
            std::format(
                "WORLD_LIGHTING_FINE geometry c0=({}, {}, {}) cc1=({}, {}, {}) cs1=({}, {}, {}) scale={}",
                decoded.m_coefficients[0u].x, decoded.m_coefficients[0u].y, decoded.m_coefficients[0u].z,
                decoded.m_coefficients[1u].x, decoded.m_coefficients[1u].y, decoded.m_coefficients[1u].z,
                decoded.m_coefficients[2u].x, decoded.m_coefficients[2u].y, decoded.m_coefficients[2u].z, scale));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (far_merge_and_clamped_edge_taps_match_the_oracle) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        CategoricalMap empty_map;
        FineInputs inputs{visuals, empty_map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, uniformSources(rgb(0.0f, 0.0f, 0.0f)));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Three complete schedule blocks so probe (0,0) is re-visited after its
        // parents are populated; a large injected dt makes each accepted frame
        // replace history with its fresh reconstruction (weight rounds to one),
        // so the last field is frame 11's reconstruction.
        for (u64 frame = 0u; frame < 12u; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, static_cast<double>(frame) * 20.0, 12, frame + 1u);
        }

        const auto record = readFineRecord(fixture, pass, 0u, 0u);
        const auto l1 = readWords(fixture, pass.cascadeIntervalsForTest(), 0u, lighting::kCascadeLevelBytes);
        const std::vector<u8> occupancy(kCells, 0u);
        const std::vector<u32> source(kCells, 0u);
        const FineOracleInputs oracle_inputs{occupancy, source, &l1, kEdge};
        const auto expected = fineConnectorOracle(0u, 0u, 11u, oracle_inputs);
        assertFineMatchesOracle(record, expected);

        // The world-edge probe clamps all four L1 taps to (0,0); the repeated
        // taps keep their ordinary bilinear weights, which sum to one.
        const auto taps = fineTaps(0u, 0u);
        float total = 0.0f;
        for (const FineTap &tap: taps) {
            PPR_TEST_ASSERT(tap.m_x == 0u and tap.m_y == 0u);
            total += tap.m_weight;
        }
        PPR_TEST_ASSERT(std::abs(total - 1.0f) <= 1.0e-6f);

        // An empty near segment leaves Tnear=1, so the stored L1 interval is the
        // whole sample; it must be nonzero for this sky hour.
        u32 nonzero_far = 0u;
        for (u32 phi = 0u; phi < 16u; ++phi) {
            const lighting::Interval far = fineFarInterval(oracle_inputs, 0u, 0u, phi, true);
            if (far.m_radiance.x > 0.0f or far.m_radiance.y > 0.0f or far.m_radiance.z > 0.0f) {
                ++nonzero_far;
            }
        }

        PPR_TEST_ASSERT(nonzero_far > 0u);
        PPR_TEST_ASSERT(lighting::unpackCH(record).m_coefficients[0u].x > 0.0f);
        _.log(
            std::format(
                "WORLD_LIGHTING_FINE far_merge edge_taps=clamped weights=1 nonzero_far={} oracle=match", nonzero_far));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (window_applied_once_across_multiple_ema_frames) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        QuadrantFixture quadrant;
        FineInputs inputs{visuals, quadrant.m_map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, quadrant.m_sources);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);
        const auto first = readFineRecord(fixture, pass, 100u, 100u);
        const lighting::CH first_decoded = lighting::unpackCH(first);
        PPR_TEST_ASSERT(first.m_words[1u] != 0u);

        // Black metallic sources keep every cell occupied but make the fresh
        // reconstruction exactly zero, so each EMA step is history*(1-alpha).
        std::array<lighting::SourceMaterial, kMaterialCount> black_sources{};
        inputs.setSources(fixture, black_sources);

        constexpr double kFrameDelta = 0.25;
        const float alpha = lighting::fineAccumulationWeight(kFrameDelta);
        PPR_TEST_ASSERT(alpha > 0.0f and alpha < 1.0f);

        runFrame(fixture, pass, inputs.inputs(), 1u, kFrameDelta, -1, 2u);
        const auto second = readFineRecord(fixture, pass, 100u, 100u);
        const lighting::CH second_decoded = lighting::unpackCH(second);

        runFrame(fixture, pass, inputs.inputs(), 2u, 2.0 * kFrameDelta, -1, 3u);
        const auto third = readFineRecord(fixture, pass, 100u, 100u);
        const lighting::CH third_decoded = lighting::unpackCH(third);

        const auto assertStep = [&](const lighting::CH &previous, const lighting::PackedCH &current_packed,
                                    const lighting::CH &current, const char *const label) {
            const float scale = std::bit_cast<float>(current_packed.m_words[1u]);
            const float tolerance = 0.55f * scale + 2.0e-4f;
            const float retention = 1.0f - alpha;
            PPR_TEST_ASSERT(closeRGB9E5(current.m_coefficients[0u], previous.m_coefficients[0u] * retention, 5.0e-3f));
            for (u32 coefficient = 1u; coefficient < 9u; ++coefficient) {
                const float3 expected = previous.m_coefficients[coefficient] * retention;
                const float3 actual = current.m_coefficients[coefficient];
                PPR_TEST_ASSERT(std::abs(actual.x - expected.x) <= tolerance);
                PPR_TEST_ASSERT(std::abs(actual.y - expected.y) <= tolerance);
                PPR_TEST_ASSERT(std::abs(actual.z - expected.z) <= tolerance);
            }

            _.log(std::format("WORLD_LIGHTING_FINE window_once {} retention={}", label, retention));
        };

        assertStep(first_decoded, second, second_decoded, "frame1");
        assertStep(second_decoded, third, third_decoded, "frame2");

        // Direct ratio on the strongest first-frame AC component. A second
        // Lanczos window on load/blend would shrink this by window(m), which is
        // far outside the quantization bound.
        float strongest = 0.0f;
        u32 strongest_coefficient = 0u;
        u32 strongest_channel = 0u;
        for (u32 coefficient = 1u; coefficient < 9u; ++coefficient) {
            const float3 value = first_decoded.m_coefficients[coefficient];
            const std::array<float, 3u> channels{value.x, value.y, value.z};
            for (u32 channel = 0u; channel < channels.size(); ++channel) {
                if (std::abs(channels[channel]) > std::abs(strongest)) {
                    strongest = channels[channel];
                    strongest_coefficient = coefficient;
                    strongest_channel = channel;
                }
            }
        }

        PPR_TEST_ASSERT(std::abs(strongest) > 1.0e-4f);
        const float3 second_value = second_decoded.m_coefficients[strongest_coefficient];
        const float second_component = strongest_channel == 0u
                                           ? second_value.x
                                           : (strongest_channel == 1u ? second_value.y : second_value.z);
        const float ratio = second_component / strongest;
        PPR_TEST_ASSERT(std::abs(ratio - (1.0f - alpha)) <= 0.025f);
        _.log(
            std::format(
                "WORLD_LIGHTING_FINE window_once ratio={} expected={} coefficient={} channel={}", ratio, 1.0f - alpha,
                strongest_coefficient, strongest_channel));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (retained_l1_direction_identity_uses_the_probe_trace_frame) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

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
        _.log(
            std::format(
                "WORLD_LIGHTING_FINE retained_direction probes={} retained={} frame=current_vs_trace", queries.size(),
                retained_directions));
    };

    PPR_UNIT_TEST (fine_writes_all_bytes_and_reads_only_its_own_prior) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        // Both probed blocks are opened so their near traces reach the uniform
        // emitter ring instead of their black self-occluders.
        map.clearProbeBlock(210u, 310u);
        map.clearProbeBlock(211u, 310u);
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

        _.log(std::format("WORLD_LIGHTING_FINE in_place all_32_bytes=true own_prior_only=true single_allocation=true"));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (source_reads_precede_fine_writes_across_successive_frames) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kDiffuse);
        // A 4x4 vacuum pocket opens the probe's own block; the solid cell read
        // below keeps only the pocket-side taps, so its incident mean is the
        // uniform emitter field and not the solid's own bounce.
        map.clearRect(512u, 512u, 515u, 515u);
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
            readWords(fixture, pass.sourceCellsForTest(), cellIndex(516u, 513u) * 4u, 4u)[0u]);
        PPR_TEST_ASSERT(closeRGB9E5(cold_source, emission));

        const float3 field_mean = fineAngularMean(readFineRecord(fixture, pass, 256u, 256u));
        PPR_TEST_ASSERT(closeRGB9E5(field_mean, emission));

        // Frame 1's source must read the frame-0 field, not the field that this
        // submission is about to write. With unit diffuse the expected source is
        // emission + emission = 2*emission; reading the new field would give 3x.
        runFrame(fixture, pass, inputs.inputs(), 1u, 0.25, -1, 2u);
        const auto warm_source = lighting::unpackRGB9E5(
            readWords(fixture, pass.sourceCellsForTest(), cellIndex(516u, 513u) * 4u, 4u)[0u]);
        PPR_TEST_ASSERT(closeRGB9E5(warm_source, emission * 2.0f));
        PPR_TEST_ASSERT(not closeRGB9E5(warm_source, emission * 3.0f));

        _.log(std::format("WORLD_LIGHTING_FINE barriers source_pre_update=true fine_then_next_source=true"));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // probe self-exclusion
    // ------------------------------------------------------------------

    // A solid 10x10 metal region with a distinct 2x2 block on its corner. The
    // probe at (100,100) sits on the shared corner of those four cells, so the
    // near trace traverses them and terminates on the surrounding metal ring.
    [[nodiscard]] CategoricalMap ownBlockMap(const u32 block_material) {
        CategoricalMap map;
        map.fillRect(196u, 196u, 205u, 205u, kMetal);
        map.fillRect(200u, 200u, 201u, 201u, block_material);
        return map;
    }

    [[nodiscard]] std::array<lighting::SourceMaterial, kMaterialCount> twoRegionSources(
        const float3 room_emission, const u32 block_material, const float3 block_emission, const float block_metallic,
        const float3 block_tint) {
        std::array<lighting::SourceMaterial, kMaterialCount> sources{};
        const auto room = lighting::makeSourceMaterial(
            room_emission, 1.0, rgb(1.0f, 1.0f, 1.0f), rgb(1.0f, 1.0f, 1.0f), 1.0);
        const auto block = lighting::makeSourceMaterial(
            block_emission, 1.0, block_tint, rgb(1.0f, 1.0f, 1.0f), block_metallic);
        PPR_TEST_ASSERT(room.has_value() and block.has_value());
        for (auto &source: sources) {
            source = *room;
        }

        sources[block_material] = *block;
        return sources;
    }

    PPR_UNIT_TEST (occupied_own_block_is_an_opaque_black_occluder) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        const float3 room_emission = rgb(0.0f, 0.0f, 2.0f);
        const float3 own_emission = rgb(4.0f, 0.0f, 0.0f);

        // The block is occupied, so every near direction terminates on the
        // probe's own surface with zero radiance and zero transmittance.
        CategoricalMap map = ownBlockMap(kRed);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, twoRegionSources(room_emission, kRed, own_emission, 1.0f, rgb(1.0f, 1.0f, 1.0f)));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);
        const auto blocked = readFineRecord(fixture, pass, 100u, 100u);
        for (const u32 word: blocked.m_words) {
            PPR_TEST_ASSERT(word == 0u);
        }

        // The same probe with a vacant block sees the room through an ordinary
        // hit: only occupied block cells occlude.
        CategoricalMap vacant_map = map;
        vacant_map.clearProbeBlock(100u, 100u);
        FineInputs vacant_inputs{visuals, vacant_map};
        vacant_inputs.uploadCommon(fixture);
        vacant_inputs.setSources(
            fixture, twoRegionSources(room_emission, kRed, own_emission, 1.0f, rgb(1.0f, 1.0f, 1.0f)));
        runFrame(fixture, pass, vacant_inputs.inputs(), 1u, 20.0, -1, 2u);
        const float3 open = fineAngularMean(readFineRecord(fixture, pass, 100u, 100u));
        PPR_TEST_ASSERT(closeRGB9E5(open, room_emission));
        PPR_TEST_ASSERT(not closeRGB9E5(open, own_emission));

        _.log("WORLD_LIGHTING_FINE self_occluder occupied_block=black vacant_block=ordinary_hit");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (world_edge_solid_probe_is_black) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, uniformSources(rgb(1.0f, 1.0f, 1.0f)));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);

        // No domain-edge exception: an occupied own block is a black
        // self-occluder at the world edge exactly as in the interior.
        for (const std::array<u32, 2u> probe: {std::array<u32, 2u>{0u, 0u}, std::array<u32, 2u>{511u, 511u}}) {
            const auto record = readFineRecord(fixture, pass, probe[0u], probe[1u]);
            for (const u32 word: record.m_words) {
                PPR_TEST_ASSERT(word == 0u);
            }
        }

        _.log("WORLD_LIGHTING_FINE self_occluder domain_edge=black");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // wall opacity and one-sided illumination
    // ------------------------------------------------------------------

    // A full-width two-cell wall (rows 200/201) between a lit vacuum room above
    // with a nearby metallic emitter band and a dark room below. The dark room
    // is sealed by a zero-source shell (the wall itself closes its top), so
    // every shadow-side ray terminates on a black cell: its probes carry no
    // ambient and the shadow-side cell's incident mean is exactly zero. The
    // full-width wall leaves no path around its ends either. The wall is red
    // diffuse.
    [[nodiscard]] CategoricalMap twoRoomWallMap() {
        CategoricalMap map;
        map.fillRect(0u, 200u, 1023u, 201u, kDiffuse);

        // Lit room: metallic emitter band within the near reach of the wall.
        map.fillRect(190u, 205u, 210u, 206u, kMetal);

        // Sealed dark room around the shadow-side probes: interior x 199..201,
        // y 198..199; zero-source shell on the sides and floor, the wall above.
        map.fillRect(197u, 196u, 198u, 199u, kBlue);
        map.fillRect(202u, 196u, 203u, 199u, kBlue);
        map.fillRect(197u, 196u, 203u, 197u, kBlue);
        return map;
    }

    PPR_UNIT_TEST (two_cell_wall_keeps_the_shadow_side_dark) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        const float3 emitter = rgb(1.0f, 1.0f, 1.0f);
        const float3 red_tint = rgb(0.7f, 0.05f, 0.05f);

        CategoricalMap map = twoRoomWallMap();
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        auto sources = twoRegionSources(emitter, kDiffuse, rgb(0.0f, 0.0f, 0.0f), 0.0f, red_tint);
        const auto black_source = lighting::makeSourceMaterial(
            rgb(0.0f, 0.0f, 0.0f), 0.0, rgb(1.0f, 1.0f, 1.0f), rgb(1.0f, 1.0f, 1.0f), 1.0);
        PPR_TEST_ASSERT(black_source.has_value());
        sources[kBlue] = *black_source;
        inputs.setSources(fixture, sources);
        const float3 diffuse = lighting::unpackRGB9E5(sources[kDiffuse].m_diffuse);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Frame 0 writes the incident field; frame 1's source reads it.
        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);
        const float3 left = fineAngularMean(readFineRecord(fixture, pass, 99u, 101u));
        const float3 right = fineAngularMean(readFineRecord(fixture, pass, 100u, 101u));

        // The shadow-side room probe is exactly black: no light crosses the
        // two-cell wall.
        const auto dark_probe = readFineRecord(fixture, pass, 100u, 99u);
        for (const u32 word: dark_probe.m_words) {
            PPR_TEST_ASSERT(word == 0u);
        }

        const auto sourceAt = [&](const u32 x, const u32 y) {
            return lighting::unpackRGB9E5(
                readWords(fixture, pass.sourceCellsForTest(), static_cast<u64>(cellIndex(x, y)) * 4u, 4u)[0u]);
        };

        runFrame(fixture, pass, inputs.inputs(), 1u, 0.5, -1, 2u);

        // The lit-side cell keeps only its two room-side taps: its source is
        // exactly rho * the renormalised incident mean, with the interior wall
        // tap dropped, and it stays red-dominant.
        const float3 lit = sourceAt(200u, 201u);
        const float3 incident = left * 0.25f + right * 0.75f;
        PPR_TEST_ASSERT(closeRGB9E5(lit, diffuse * incident, 1.0e-2f));
        PPR_TEST_ASSERT(lit.x > 0.0f);
        PPR_TEST_ASSERT(lit.x > 10.0f * lit.y and lit.x > 10.0f * lit.z);
        PPR_TEST_ASSERT(lit.x <= diffuse.x * emitter.x + 2.0e-3f);

        // The shadow-side cell stays exactly black, so the ratio is zero.
        const float3 dark = sourceAt(200u, 200u);
        PPR_TEST_ASSERT(dark.x == 0.0f and dark.y == 0.0f and dark.z == 0.0f);
        PPR_TEST_ASSERT(dark.x < 0.05f * lit.x);

        // A later accepted frame leaves the shadow side dark and the lit side
        // still driven by the room, inside rho * L_room.
        runFrame(fixture, pass, inputs.inputs(), 2u, 1.0, -1, 3u);
        const float3 dark_warm = sourceAt(200u, 200u);
        PPR_TEST_ASSERT(dark_warm.x == 0.0f and dark_warm.y == 0.0f and dark_warm.z == 0.0f);
        const float3 lit_warm = sourceAt(200u, 201u);
        PPR_TEST_ASSERT(lit_warm.x > 0.0f);
        PPR_TEST_ASSERT(lit_warm.x <= diffuse.x * emitter.x + 2.0e-3f);

        _.log(
            std::format(
                "WORLD_LIGHTING_FINE wall two_cell lit=({}, {}, {}) dark=({}, {}, {}) ratio=zero", lit.x, lit.y, lit.z,
                dark.x, dark.y, dark.z));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (one_cell_wall_blocks_the_opposite_room) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        const float3 lit = rgb(2.0f, 2.0f, 0.0f);
        const float3 opposite = rgb(0.0f, 0.0f, 4.0f);

        // One opaque black metal wall row (full width, so nothing passes around
        // its ends) between a yellow lit emitter above and a sealed dark room
        // below with a blue emitter inside it. The wall and the room shell have
        // zero source, so any colour from the far side can only arrive by a ray
        // crossing the one-cell wall; no coarse ambient can enter the room.
        CategoricalMap map;
        map.fillRect(0u, 200u, 1023u, 200u, kRed);
        map.fillRect(190u, 203u, 210u, 204u, kMetal);
        map.fillRect(199u, 197u, 201u, 197u, kBlue);
        map.fillRect(197u, 194u, 198u, 199u, kGreen);
        map.fillRect(202u, 194u, 203u, 199u, kGreen);
        map.fillRect(197u, 194u, 203u, 195u, kGreen);

        std::array<lighting::SourceMaterial, kMaterialCount> sources{};
        const auto store = [&](const u32 material, const float3 emission) {
            const auto record = lighting::makeSourceMaterial(
                emission, 1.0, rgb(1.0f, 1.0f, 1.0f), rgb(1.0f, 1.0f, 1.0f), 1.0);
            PPR_TEST_ASSERT(record.has_value());
            sources[material] = *record;
        };
        store(kMetal, lit);
        store(kBlue, opposite);
        store(kRed, rgb(0.0f, 0.0f, 0.0f));
        store(kGreen, rgb(0.0f, 0.0f, 0.0f));

        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, sources);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0.0, -1, 1u);

        // The lit-side face probe sees its own room and no blue: its downward
        // rays terminate on the single wall cell instead of crossing it.
        const float3 lit_side = fineAngularMean(readFineRecord(fixture, pass, 100u, 100u));
        PPR_TEST_ASSERT(lit_side.x > 0.0f);
        PPR_TEST_ASSERT(lit_side.z <= 1.0e-4f);

        // The opposite face probe sees its own blue room and no yellow.
        const float3 dark_side = fineAngularMean(readFineRecord(fixture, pass, 100u, 99u));
        PPR_TEST_ASSERT(dark_side.z > 0.0f);
        PPR_TEST_ASSERT(dark_side.x <= 1.0e-4f);

        _.log(
            std::format(
                "WORLD_LIGHTING_FINE wall one_cell lit_side=({}, {}, {}) dark_side=({}, {}, {}) cross=none", lit_side.x,
                lit_side.y, lit_side.z, dark_side.x, dark_side.y, dark_side.z));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (mid_wall_probe_is_black_with_no_far_leak) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        CategoricalMap map = twoRoomWallMap();
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        inputs.setSources(
            fixture,
            twoRegionSources(rgb(1.0f, 1.0f, 1.0f), kDiffuse, rgb(0.0f, 0.0f, 0.0f), 0.0f, rgb(0.7f, 0.05f, 0.05f)));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Probe (100,100) is the wall's internal corner: all four own-block cells
        // are occupied, so every direction is a terminal black hit. Warm frames
        // populate the far intervals; zero transmittance keeps them out.
        for (u32 frame = 0u; frame < 3u; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, 0.5 * static_cast<double>(frame), -1, frame + 1u);
            const auto record = readFineRecord(fixture, pass, 100u, 100u);
            for (const u32 word: record.m_words) {
                PPR_TEST_ASSERT(word == 0u);
            }
        }

        // The lit wall cell carries real source, so the L1/far store does hold
        // non-zero radiance that the black near interval must suppress; the
        // probe stayed exactly zero above.
        const float3 lit = lighting::unpackRGB9E5(
            readWords(fixture, pass.sourceCellsForTest(), static_cast<u64>(cellIndex(200u, 201u)) * 4u, 4u)[0u]);
        PPR_TEST_ASSERT(lit.x > 0.0f);

        _.log("WORLD_LIGHTING_FINE wall mid_probe black=true far_leak=none frames=3");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (occupied_cell_drops_interior_taps_and_has_zero_incident) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        const float3 emission = rgb(0.5f, 0.75f, 1.0f);

        // A solid diffuse field with a large supplied prior DC: every cell's
        // four corner taps are interior probes, so the filter drops them all,
        // the incident mean stays zero and the source is the authored emission
        // on every frame. Without the drop the huge field would dominate.
        CategoricalMap map;
        map.fill(kDiffuse);
        FineInputs inputs{visuals, map};
        inputs.uploadCommon(fixture);
        inputs.setSources(fixture, uniformSources(emission, 0.0f));
        inputs.setUniformPriorCH(fixture, rgb(50.0f, 50.0f, 50.0f));

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        for (u32 frame = 0u; frame < 2u; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, 0.5 * static_cast<double>(frame), -1, frame + 1u);
            const float3 source = lighting::unpackRGB9E5(
                readWords(fixture, pass.sourceCellsForTest(), static_cast<u64>(cellIndex(512u, 512u)) * 4u, 4u)[0u]);
            PPR_TEST_ASSERT(closeRGB9E5(source, emission));

            const auto record = readFineRecord(fixture, pass, 256u, 256u);
            for (const u32 word: record.m_words) {
                PPR_TEST_ASSERT(word == 0u);
            }
        }

        _.log("WORLD_LIGHTING_FINE source interior_taps=dropped incident=zero no_fixed_point");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (fine_range_failure_reports_status_and_containment) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto visuals = makePalette();
        CategoricalMap map;
        map.fill(kMetal);
        // An interior probe with an open block: every near direction reaches the
        // 60,000 emitter ring, so its C0 is far over the DC limit. An occupied
        // own block would be a black self-occluder and never reach the limit.
        constexpr u32 kRangeProbeX = 100u;
        constexpr u32 kRangeProbeY = 100u;
        constexpr u32 kRangeInvocation = kRangeProbeY * kFineEdge + kRangeProbeX;
        map.clearProbeBlock(kRangeProbeX, kRangeProbeY);
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
            PPR_TEST_ASSERT(words[kNumericStageFine * kNumericStatusWords + 1u] == kRangeInvocation);
            for (u32 word = 2u; word < kNumericStatusWords; ++word) {
                PPR_TEST_ASSERT(words[kNumericStageFine * kNumericStatusWords + word] == 0u);
            }
        }

        PPR_TEST_ASSERT(pass.collectCompleted(1u) == std::errc::result_out_of_range);
        const auto contained = readFineRecord(fixture, pass, kRangeProbeX, kRangeProbeY);
        for (const u32 word: contained.m_words) {
            PPR_TEST_ASSERT(word == 0u);
        }

        _.log(std::format("WORLD_LIGHTING_FINE status range=7 index={} containment=zero", kRangeInvocation));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (angular_mean_helper_matches_bilinear_oracle) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

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

        _.log(std::format("WORLD_LIGHTING_FINE angular_mean y0_normalized=true extra_factors=none"));
    };

    // ------------------------------------------------------------------
    // reflected ABI
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (reflected_fine_bindings_match_the_cpu_shapes) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

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
        PPR_TEST_ASSERT(fine_constants->findFieldIndexByName("m_fine_edge", nullptr) == 2);
        PPR_TEST_ASSERT(
            fine_constants->getFieldByIndex(2u)->getTypeLayout()->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 4u);
        PPR_TEST_ASSERT(WorldLightingPass::kFineCHEdge == 512u);

        shader::ComPtr<slang::IEntryPoint> point{};
        PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("reconstructFine", point.writeRef())));

        PPR_TEST_ASSERT(WorldLightingPass::kFineCHEdge == 512u);
        PPR_TEST_ASSERT(WorldLightingPass::kFineCHRecordCount == 262'144u);
        PPR_TEST_ASSERT(WorldLightingPass::kFineCHBytes == 8'388'608u);
        PPR_TEST_ASSERT(WorldLightingPass::kFineDispatchThreads == 8u);

        _.log(
            std::format(
                "WORLD_LIGHTING_FINE reflected ch_stride=32 cascade_stride=8 constants=32 entry=reconstructFine"));
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
                detail::WorldLightingFine::connector_geometry_produces_signed_harmonics,
                detail::WorldLightingFine::far_merge_and_clamped_edge_taps_match_the_oracle,
                detail::WorldLightingFine::window_applied_once_across_multiple_ema_frames,
                detail::WorldLightingFine::retained_l1_direction_identity_uses_the_probe_trace_frame,
                detail::WorldLightingFine::fine_writes_all_bytes_and_reads_only_its_own_prior,
                detail::WorldLightingFine::source_reads_precede_fine_writes_across_successive_frames,
                detail::WorldLightingFine::occupied_own_block_is_an_opaque_black_occluder,
                detail::WorldLightingFine::world_edge_solid_probe_is_black,
                detail::WorldLightingFine::two_cell_wall_keeps_the_shadow_side_dark,
                detail::WorldLightingFine::one_cell_wall_blocks_the_opposite_room,
                detail::WorldLightingFine::mid_wall_probe_is_black_with_no_far_leak,
                detail::WorldLightingFine::occupied_cell_drops_interior_taps_and_has_zero_incident,
                detail::WorldLightingFine::fine_range_failure_reports_status_and_containment,
                detail::WorldLightingFine::angular_mean_helper_matches_bilinear_oracle,
                detail::WorldLightingFine::reflected_fine_bindings_match_the_cpu_shapes,
            });
        };
        return group;
    }
}
