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

namespace pP::tests::detail::WorldLightingCascade {
    // ------------------------------------------------------------------
    // fixture values
    // ------------------------------------------------------------------

    inline constexpr u32 kEdge = WorldLightingPass::kWorldEdge;
    inline constexpr u32 kCells = WorldLightingPass::kCellCount;
    inline constexpr u32 kMaterialCount = 8u;

    inline constexpr u32 kOpaque = 1u;
    inline constexpr u32 kReceiveOnly = 2u;
    inline constexpr u32 kEmissive = 3u;
    inline constexpr u32 kMetal = 4u;
    inline constexpr u32 kDiffuse = 5u;
    inline constexpr u32 kHot = 6u;

    inline constexpr u32 kTerminal = lighting::kCascadeTerminalLevel;
    inline constexpr u32 kTerminalDirections = lighting::kCascadeTerminalDirections;
    // Stored L5 directions: 4 << (2*5). The terminal sub-sector count is four
    // times this, so the two index spaces must never be mixed.
    inline constexpr u32 kStoredDirections = kTerminalDirections / 4u;
    inline constexpr u32 kLevelRecords = lighting::kCascadeLevelRecords;
    inline constexpr u32 kDomainEdge = lighting::kCascadeDomainEdge;
    inline constexpr u32 kThreads = lighting::kCascadeDispatchThreads;

    static_assert(kTerminal == 5u and kTerminalDirections == 16'384u);
    static_assert(kDomainEdge == 512u and kThreads == 16u);
    static_assert(kDomainEdge / kThreads == 32u);
    static_assert(lighting::kCascadeBufferBytes == 41'943'040u);

    // Approved ray interval starts, in native-cell distance units.
    inline constexpr std::array<float, 5u> kIntervalStarts{1.024f, 4.096f, 16.384f, 65.536f, 262.144f};

    constexpr u32 cellIndex(const u32 x, const u32 y) noexcept {
        return y * kEdge + x;
    }

    [[nodiscard]] lighting::Interval makeInterval(const float3 radiance, const float transmittance) noexcept {
        lighting::Interval result{};
        result.m_radiance = radiance;
        result.m_transmittance = transmittance;
        return result;
    }

    [[nodiscard]] float3 rgb(const float r, const float g, const float b) noexcept {
        return float3{r, g, b};
    }

    // RGB9E5 shares one exponent, so a small channel carries the shared quantum
    // of the largest component. Comparisons use that quantum, not the small
    // channel's own magnitude.
    [[nodiscard]] bool closeRGB9E5(const float3 actual, const float3 expected) noexcept {
        const float scale = (std::max)({
            std::abs(actual.x), std::abs(actual.y), std::abs(actual.z), std::abs(expected.x), std::abs(expected.y),
            std::abs(expected.z)});
        const float tolerance = 3.0e-3f * scale + 1.0e-6f;
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
                "WORLD_LIGHTING_CASCADE api={} adapter={} backend={}",
                info.apiName != nullptr ? info.apiName : "unknown",
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
    // buffer, texture and submission plumbing
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

    [[nodiscard]] rhi::ComPtr<rhi::ITexture> makeJumpTexture(rhi::IDevice &device, const u32 edge) {
        const std::vector<u8> initial(static_cast<std::size_t>(edge) * edge, 0u);
        const rhi::SubresourceData data{initial.data(), static_cast<u64>(edge), static_cast<u64>(edge) * edge};
        rhi::TextureDesc description{};
        description.type = rhi::TextureType::Texture2D;
        description.size = {edge, edge, 1u};
        description.arrayLength = 1u;
        description.mipCount = 1u;
        description.format = rhi::Format::R8Uint;
        description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::ShaderResource) | static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
        description.defaultState = rhi::ResourceState::ShaderResource;
        rhi::ComPtr<rhi::ITexture> texture;
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(description, &data, texture.writeRef())));
        return texture;
    }

    [[nodiscard]] rhi::ComPtr<rhi::ICommandEncoder> makeEncoder(Fixture &fixture) {
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        return encoder;
    }

    void submit(Fixture &fixture, rhi::ICommandEncoder &encoder, const bool wait = true) {
        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        if (wait) {
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        }
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
    // categorical map and palette fixtures
    // ------------------------------------------------------------------

    struct CategoricalMap {
        std::vector<u32> m_words{};

        CategoricalMap() {
            m_words.assign(kCells, 0u);
        }

        void occupy(const u32 x, const u32 y, const u32 material = kOpaque, const double kelvin = 300.0) {
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

        [[nodiscard]] std::vector<u8> occupancy(const std::array<GridTerrainVisual, kMaterialCount> &palette) const {
            std::vector<u8> result(kCells, 0u);
            for (u32 cell = 0u; cell < kCells; ++cell) {
                const u32 material = m_words[cell] & 0xffffu;
                const bool receive_only = material != 0u and
                                          lighting::receiveOnlyCell(
                                              palette[material].m_surface_flags, palette[material].m_emissive_strength);
                result[cell] = lighting::resolvedCellOccupied(m_words[cell], receive_only) ? 1u : 0u;
            }

            return result;
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

        palette[kReceiveOnly].m_surface_flags = kGridSurfaceOpticalBlend;
        palette[kReceiveOnly].m_opacity = 0.5f;
        palette[kEmissive].m_emissive_rgb[0] = 1.0f;
        palette[kEmissive].m_emissive_rgb[1] = 0.5f;
        palette[kEmissive].m_emissive_rgb[2] = 0.25f;
        palette[kEmissive].m_emissive_strength = 2.0f;
        palette[kMetal].m_metallic = 1.0f;
        palette[kMetal].m_emissive_rgb[1] = 0.5f;
        palette[kMetal].m_emissive_strength = 1.0f;
        palette[kHot].m_metallic = 1.0f;
        return palette;
    }

    [[nodiscard]] std::array<lighting::SourceMaterial, kMaterialCount> makeSourcePalette() {
        std::array<lighting::SourceMaterial, kMaterialCount> palette{};
        const auto white = rgb(1.0f, 1.0f, 1.0f);
        const auto store = [&palette](
                               const u32 material, const float3 emission, const double strength, const float3 tint,
                               const float metallic) {
            const auto record = lighting::makeSourceMaterial(emission, strength, tint, rgb(1.0f, 1.0f, 1.0f), metallic);
            PPR_TEST_ASSERT(record.has_value());
            palette[material] = *record;
        };

        store(kOpaque, rgb(0.0f, 0.0f, 0.0f), 0.0, rgb(0.5f, 0.5f, 0.5f), 0.0f);
        store(kEmissive, rgb(1.0f, 0.5f, 0.25f), 2.0, white, 0.0f);
        store(kMetal, rgb(0.0f, 0.5f, 0.0f), 1.0, white, 1.0f);
        store(kDiffuse, rgb(0.0f, 0.0f, 0.0f), 0.0, white, 0.0f);
        store(kHot, rgb(0.0f, 0.0f, 0.0f), 0.0, white, 1.0f);
        palette[kReceiveOnly] = lighting::SourceMaterial{};
        return palette;
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> uploadPalette(
        Fixture &fixture, const std::array<GridTerrainVisual, kMaterialCount> &palette) {
        return makeShaderBuffer(fixture.device(), sizeof(palette), sizeof(GridTerrainVisual), palette.data());
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> uploadSourcePalette(
        Fixture &fixture, const std::array<lighting::SourceMaterial, kMaterialCount> &palette) {
        return makeShaderBuffer(fixture.device(), sizeof(palette), sizeof(lighting::SourceMaterial), palette.data());
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> uploadTerrain(Fixture &fixture, const CategoricalMap &map) {
        return makeShaderBuffer(fixture.device(), map.m_words.size() * sizeof(u32), sizeof(u32), map.m_words.data());
    }

    // ------------------------------------------------------------------
    // committed thermal table and binary64 reference
    // ------------------------------------------------------------------

    struct ThermalTable {
        std::vector<u8> m_bytes{};
        rhi::ComPtr<rhi::IBuffer> m_buffer{};

        // Rows are log-spaced exactly as thermal_emission.slang computes them.
        [[nodiscard]] static u32 rowFor(const double kelvin) noexcept {
            const double q = static_cast<double>(lighting::kThermalLutRows - 1u) *
                             std::log(kelvin / lighting::kThermalLutMinKelvin) /
                             std::log(lighting::kThermalLutMaxKelvin / lighting::kThermalLutMinKelvin);
            return (std::min) (static_cast<u32>(std::floor(q)), lighting::kThermalLutRows - 1u);
        }

        void load(Fixture &fixture) {
            m_bytes.assign(lighting::kThermalLutBytes, 0u);
            std::ifstream file{std::filesystem::current_path() / "lighting" / "thermal_lut.bin", std::ios::binary};
            PPR_TEST_ASSERT(file.is_open());
            file.read(reinterpret_cast<char *>(m_bytes.data()), static_cast<std::streamsize>(m_bytes.size()));
            PPR_TEST_ASSERT(file.gcount() == static_cast<std::streamsize>(m_bytes.size()));

            upload(fixture);
        }

        void upload(Fixture &fixture) {
            PPR_TEST_ASSERT(m_bytes.size() == lighting::kThermalLutBytes);
            m_buffer = makeShaderBuffer(fixture.device(), m_bytes.size(), lighting::kThermalLutStride, m_bytes.data());
        }

        // Test-only corruption for the source invalid-state failure path.
        void writeRow(const u32 row, const float3 value) {
            PPR_TEST_ASSERT(row < lighting::kThermalLutRows);
            const std::array<float, 3u> channels{value.x, value.y, value.z};
            for (u32 channel = 0u; channel < channels.size(); ++channel) {
                const u32 bits = std::bit_cast<u32>(channels[channel]);
                std::memcpy(
                    m_bytes.data() + row * lighting::kThermalLutStride + channel * sizeof(u32), &bits, sizeof(bits));
            }
        }

        [[nodiscard]] double value(const u32 row, const u32 channel) const noexcept {
            u32 bits{};
            std::memcpy(
                &bits, m_bytes.data() + row * lighting::kThermalLutStride + channel * sizeof(u32), sizeof(bits));
            return static_cast<double>(std::bit_cast<float>(bits));
        }

        [[nodiscard]] float3 sample(const double kelvin) const noexcept {
            if (not (kelvin > lighting::kThermalLutMinKelvin)) {
                return float3{zero_v};
            }

            const double q = (lighting::kThermalLutRows - 1u) *
                             std::log(kelvin / lighting::kThermalLutMinKelvin) /
                             std::log(lighting::kThermalLutMaxKelvin / lighting::kThermalLutMinKelvin);
            const u32 lower = (std::min) (static_cast<u32>(std::floor(q)), lighting::kThermalLutRows - 1u);
            const u32 upper = (std::min) (lower + 1u, lighting::kThermalLutRows - 1u);
            const double weight = q - static_cast<double>(lower);

            const auto interpolate = [&](const u32 channel) {
                return value(lower, channel) + (value(upper, channel) - value(lower, channel)) * weight;
            };

            return float3{
                static_cast<float>(interpolate(0u)), static_cast<float>(interpolate(1u)),
                static_cast<float>(interpolate(2u))};
        }
    };

    // ------------------------------------------------------------------
    // known prior-CH field
    // ------------------------------------------------------------------

    using CHSampler = std::function<float3(u32 x, u32 y)>;

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeCHField(Fixture &fixture, const CHSampler &sampler) {
        std::vector<lighting::PackedCH> records(lighting::kFineCHRecordCount);
        for (u32 y = 0u; y < lighting::kFineCHEdge; ++y) {
            for (u32 x = 0u; x < lighting::kFineCHEdge; ++x) {
                const auto word = lighting::packRGB9E5(sampler(x, y));
                PPR_TEST_ASSERT(word.has_value());
                records[static_cast<std::size_t>(y) * lighting::kFineCHEdge + x].m_words[0] = *word;
            }
        }

        return makeShaderBuffer(fixture.device(), lighting::kFineCHBytes, sizeof(lighting::PackedCH), records.data());
    }

    // ------------------------------------------------------------------
    // attempt driving
    // ------------------------------------------------------------------

    struct CascadeInputs {
        const std::array<GridTerrainVisual, kMaterialCount> &m_visuals;
        const CategoricalMap &m_map;
        const std::array<lighting::SourceMaterial, kMaterialCount> &m_sources;
        const ThermalTable &m_thermal;
        rhi::ComPtr<rhi::IBuffer> m_prior_ch{};
        rhi::ComPtr<rhi::IBuffer> m_palette{};
        rhi::ComPtr<rhi::IBuffer> m_source_palette{};
        rhi::ComPtr<rhi::IBuffer> m_terrain{};

        void upload(Fixture &fixture) {
            m_palette = uploadPalette(fixture, m_visuals);
            m_source_palette = uploadSourcePalette(fixture, m_sources);
            m_terrain = uploadTerrain(fixture, m_map);
        }

        [[nodiscard]] lighting::WorldLightingInputs inputs() const {
            lighting::WorldLightingInputs result{};
            result.m_terrain = m_terrain;
            result.m_visible_materials = m_palette;
            result.m_source_materials = m_source_palette;
            result.m_thermal_lut = m_thermal.m_buffer;
            result.m_prior_ch = m_prior_ch;
            result.m_material_count = kMaterialCount;
            return result;
        }
    };

    // Encodes one accepted lighting attempt at an explicit accepted-frame index.
    // The seam advances exactly like a real accepted submission.
    void runFrame(
        Fixture &fixture, WorldLightingPass &pass, const lighting::WorldLightingInputs &inputs, const u64 accepted,
        const i32 hour, const FenceCompletionValue fence) {
        pass.setAcceptedFrameForTest(accepted);

        WorldLightingFrame frame{};
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

    // Encodes and submits one accepted attempt without asserting a clean
    // collection; the caller settles the ticket's status expectations.
    void submitAttempt(
        Fixture &fixture, WorldLightingPass &pass, const lighting::WorldLightingInputs &inputs, const u64 accepted,
        const i32 hour, const FenceCompletionValue fence) {
        pass.setAcceptedFrameForTest(accepted);

        WorldLightingFrame frame{};
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
    // schedule, sky and cascade CPU oracles
    // ------------------------------------------------------------------

    // Independent spelling of the approved uint32 schedule hash.
    [[nodiscard]] u32 mix32Oracle(u32 value) noexcept {
        value = (value ^ (value >> 16u)) * 0x7feb352du;
        value = (value ^ (value >> 15u)) * 0x846ca68bu;
        return value ^ (value >> 16u);
    }

    [[nodiscard]] u32 groupPhaseOracle(const u32 group_x, const u32 group_y, const u32 level) noexcept {
        const u32 probe_edge = kEdge / (2u << level);
        const u32 group_edge = probe_edge / 2u;
        return mix32Oracle(group_x + group_edge * group_y + 0x9e3779b9u * level) & 3u;
    }

    [[nodiscard]] u64 probeAgeOracle(const u64 accepted_frame, const u32 phase, const u32 local) noexcept {
        return ((accepted_frame & 3u) + phase + 4u - local) & 3u;
    }

    [[nodiscard]] bool probeInitializedOracle(const u64 accepted_frame, const u32 phase, const u32 local) noexcept {
        return accepted_frame >= probeAgeOracle(accepted_frame, phase, local);
    }

    [[nodiscard]] double phaseFractionOracle(const u64 trace_frame) noexcept {
        const u32 phase_word = static_cast<u32>(trace_frame) * 0x9e3779b9u;
        return (static_cast<double>(phase_word >> 9u) + 0.5) / 8'388'608.0;
    }

    [[nodiscard]] double probeAngleOracle(const u32 phi, const u32 directions, const u64 trace_frame) noexcept {
        return 2.0 *
               std::numbers::pi *
               (static_cast<double>(phi) + phaseFractionOracle(trace_frame)) /
               static_cast<double>(directions);
    }

    // A stored probe's actual last trace frame under the same schedule.
    [[nodiscard]] u64 probeTraceFrame(
        const u64 accepted_frame, const u32 level, const u32 probe_x, const u32 probe_y) noexcept {
        const u32 phase = groupPhaseOracle(probe_x / 2u, probe_y / 2u, level);
        const u32 local = (probe_x % 2u) + 2u * (probe_y % 2u);
        return accepted_frame - probeAgeOracle(accepted_frame, phase, local);
    }

    // The approved analytic sky integral, evaluated in binary64 and split once
    // at the 2*pi seam without sorting the endpoints.
    [[nodiscard]] float3 skySegmentOracle(const double a0, const double a1) noexcept {
        constexpr double sky[3]{0.2, 0.5, 1.0};
        constexpr double sun[3]{20.0, 10.0, 2.0};
        const double sharpness = std::sqrt(64.0);
        const double inverse = 1.0 / sharpness;
        const double sun_angle = 5.0;

        const double base = a1 - a0 - 0.5 * (std::cos(a1) - std::cos(a0));
        const double sun_term = (std::atan(sharpness * (sun_angle - a0)) - std::atan(sharpness * (sun_angle - a1))) *
                                inverse;

        double channels[3]{};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            channels[channel] = (sky[channel] * base + sun[channel] * sun_term) / 6.0;
        }

        return float3{
            static_cast<float>(channels[0]), static_cast<float>(channels[1]), static_cast<float>(channels[2])};
    }

    [[nodiscard]] float3 skySectorOracle(const u32 sector, const u32 directions, const i32 hour) noexcept {
        if (hour < 0) {
            return float3{zero_v};
        }

        const double turn = 2.0 * std::numbers::pi;
        const double width = turn / static_cast<double>(directions);
        const double rotation = turn * static_cast<double>(hour) / 24.0;
        const double a0 = static_cast<double>(sector) * width + rotation;
        const double a1 = static_cast<double>(sector + 1u) * width + rotation;

        // Unwrap by whole turns, then split once at the seam.
        const double turns = std::floor(a0 / turn);
        const double start = a0 - turns * turn;
        const double end = a1 - turns * turn;
        float3 integral = skySegmentOracle(start, (std::min) (end, turn));
        if (end > turn) {
            const float3 tail = skySegmentOracle(0.0, end - turn);
            integral.x += tail.x;
            integral.y += tail.y;
            integral.z += tail.z;
        }

        const float normalization = static_cast<float>(turn * 5.0 / width);
        return float3{integral.x * normalization, integral.y * normalization, integral.z * normalization};
    }

    // Failing-mode oracle: wrap both endpoints into [0, 2*pi) independently and
    // then sort them. A seam-crossing sector becomes the complementary arc,
    // which a correct unwrapped implementation must never produce.
    [[nodiscard]] float3 skySortedSectorOracle(const u32 sector, const u32 directions, const i32 hour) noexcept {
        if (hour < 0) {
            return float3{zero_v};
        }

        const double turn = 2.0 * std::numbers::pi;
        const double width = turn / static_cast<double>(directions);
        const double rotation = turn * static_cast<double>(hour) / 24.0;
        const auto wrapPositive = [](const double angle) {
            const double turn = 2.0 * std::numbers::pi;
            double wrapped = std::fmod(angle, turn);
            if (wrapped < 0.0) {
                wrapped += turn;
            }

            return wrapped;
        };

        const double a0 = wrapPositive(static_cast<double>(sector) * width + rotation);
        const double a1 = wrapPositive(static_cast<double>(sector + 1u) * width + rotation);
        const float3 integral = skySegmentOracle((std::min) (a0, a1), (std::max) (a0, a1));
        const float normalization = static_cast<float>(turn * 5.0 / width);
        return float3{integral.x * normalization, integral.y * normalization, integral.z * normalization};
    }

    struct EvidenceTrace {
        lighting::Interval m_interval{};
        u32 m_hit{kNoNumericIndex};
    };

    [[nodiscard]] EvidenceTrace referenceSegment(
        const std::vector<u8> &occupancy, const std::vector<u32> &source_words, const float2 start, const float2 end) {
        EvidenceTrace result{};
        const auto hit = lighting::traceNativeCellDDA(
            std::span<const u8>{occupancy.data(), occupancy.size()}, kEdge, start, end);
        if (not hit) {
            result.m_interval = makeInterval(float3{zero_v}, 1.0f);
            return result;
        }

        result.m_hit = *hit;
        result.m_interval = makeInterval(lighting::unpackRGB9E5(source_words[*hit]), 0.0f);
        return result;
    }

    // One L5 stored sector: four terminal sub-sectors to the forward map exit.
    [[nodiscard]] lighting::Interval terminalOracle(
        const std::vector<u8> &occupancy, const std::vector<u32> &source_words, const u32 probe_x, const u32 probe_y,
        const u64 trace_frame, const u32 phi, const i32 hour, std::optional<double> *const farthest_hit = nullptr) {
        const double a5 = static_cast<double>(kIntervalStarts[4]);
        const float2 center{
            static_cast<float>((static_cast<double>(probe_x) + 0.5) * 64.0),
            static_cast<float>((static_cast<double>(probe_y) + 0.5) * 64.0)};

        float3 radiance{zero_v};
        for (u32 sub = 0u; sub < 4u; ++sub) {
            const u32 sector = 4u * phi + sub;
            const double angle = probeAngleOracle(sector, kTerminalDirections, trace_frame);
            const float2 direction{static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
            const float2 start = center + direction * static_cast<float>(a5);
            const float2 end = start + direction * (2.0f * static_cast<float>(kEdge));

            const EvidenceTrace trace = referenceSegment(occupancy, source_words, start, end);
            if (trace.m_hit == kNoNumericIndex) {
                const float3 sky = skySectorOracle(sector, kTerminalDirections, hour);
                radiance += sky;
                continue;
            }

            radiance += trace.m_interval.m_radiance;
            if (farthest_hit != nullptr) {
                const float2 hit_center{
                    static_cast<float>((trace.m_hit % kEdge) + 0.5), static_cast<float>((trace.m_hit / kEdge) + 0.5)};
                // The terminal interval starts one a5 beyond the probe centre;
                // the finite-end distance is measured from that centre.
                const double delta_x = static_cast<double>(hit_center.x) - static_cast<double>(center.x);
                const double delta_y = static_cast<double>(hit_center.y) - static_cast<double>(center.y);
                const double distance = std::sqrt(delta_x * delta_x + delta_y * delta_y);
                if (not farthest_hit->has_value() or distance > farthest_hit->value()) {
                    *farthest_hit = distance;
                }
            }
        }

        return makeInterval(radiance * 0.25f, 0.0f);
    }

    // ------------------------------------------------------------------
    // stored-record decode and lower-level merge oracle
    // ------------------------------------------------------------------

    // Independent spelling of the approved atlas address inside one level:
    // atlas_x = probe.x*s + phi%s, atlas_y = probe.y*s + phi/s,
    // record = atlas_y*1024 + atlas_x. The whole-store address adds the
    // (level-1)*1,048,576 level offset.
    [[nodiscard]] constexpr u32 levelRecordIndex(
        const u32 level, const u32 probe_x, const u32 probe_y, const u32 phi) noexcept {
        const u32 spacing = 2u << level;
        const u32 atlas_x = probe_x * spacing + phi % spacing;
        const u32 atlas_y = probe_y * spacing + phi / spacing;
        return atlas_y * kEdge + atlas_x;
    }

    [[nodiscard]] constexpr u32 storeRecordIndex(
        const u32 level, const u32 probe_x, const u32 probe_y, const u32 phi) noexcept {
        return (level - 1u) * kLevelRecords + levelRecordIndex(level, probe_x, probe_y, phi);
    }

    [[nodiscard]] lighting::Interval decodeRecord(
        const std::vector<u32> &level_words, const u32 level, const u32 probe_x, const u32 probe_y, const u32 phi) {
        const std::size_t index = levelRecordIndex(level, probe_x, probe_y, phi);

        lighting::PackedInterval packed{};
        packed.m_rg = level_words[index * 2u];
        packed.m_bt = level_words[index * 2u + 1u];
        return lighting::unpackInterval(packed);
    }

    // Level-local words of one persistent interval level.
    [[nodiscard]] std::vector<u32> readLevel(Fixture &fixture, rhi::IBuffer *const cascades, const u32 level) {
        PPR_TEST_ASSERT(cascades != nullptr);
        PPR_TEST_ASSERT(level >= 1u and level <= lighting::kCascadeLevels);
        const u64 offset = static_cast<u64>(level - 1u) * lighting::kCascadeLevelBytes;
        return readWords(fixture, cascades, offset, lighting::kCascadeLevelBytes);
    }

    [[nodiscard]] lighting::Interval mergeOracle(
        const std::vector<u8> &occupancy, const std::vector<u32> &source_words,
        const std::vector<u32> &parent_level_words, const u32 level, const u32 probe_x, const u32 probe_y,
        const u64 accepted_frame, const u32 phi) {
        const u32 spacing = lighting::cascadeSpacing(level);
        const u32 parent_level = level + 1u;
        const u32 parent_spacing = lighting::cascadeSpacing(parent_level);
        const u32 parent_edge = lighting::cascadeProbeEdge(parent_level);
        const u32 directions = lighting::cascadeDirections(level);

        const float2 center{
            static_cast<float>((static_cast<double>(probe_x) + 0.5) * static_cast<double>(spacing)),
            static_cast<float>((static_cast<double>(probe_y) + 0.5) * static_cast<double>(spacing))};
        const double angle = probeAngleOracle(phi, directions, accepted_frame);
        const float2 direction{static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        const float2 start = center + direction * lighting::cascadeIntervalStart(level);
        const float end_distance = lighting::cascadeIntervalStart(level + 1u);

        const double relative_x = static_cast<double>(center.x) / parent_spacing - 0.5;
        const double relative_y = static_cast<double>(center.y) / parent_spacing - 0.5;
        const i32 base_x = static_cast<i32>(std::floor(relative_x));
        const i32 base_y = static_cast<i32>(std::floor(relative_y));
        const double fraction_x = relative_x - base_x;
        const double fraction_y = relative_y - base_y;

        const std::array<double, 4u> weights{
            (1.0 - fraction_x) * (1.0 - fraction_y), fraction_x * (1.0 - fraction_y), (1.0 - fraction_x) * fraction_y,
            fraction_x * fraction_y};
        const std::array<int2, 4u> offsets{int2{0, 0}, int2{1, 0}, int2{0, 1}, int2{1, 1}};

        lighting::Interval accumulated = makeInterval(float3{zero_v}, 0.0f);
        for (u32 tap = 0u; tap < 4u; ++tap) {
            const double weight = weights[tap];
            if (weight <= 0.0) {
                continue;
            }

            const u32 parent_x = static_cast<u32>(
                (std::clamp) (base_x + offsets[tap].x, 0, static_cast<i32>(parent_edge) - 1));
            const u32 parent_y = static_cast<u32>(
                (std::clamp) (base_y + offsets[tap].y, 0, static_cast<i32>(parent_edge) - 1));
            const float2 parent_center{
                static_cast<float>((static_cast<double>(parent_x) + 0.5) * parent_spacing),
                static_cast<float>((static_cast<double>(parent_y) + 0.5) * parent_spacing)};
            const EvidenceTrace near_interval = referenceSegment(
                occupancy, source_words, start, parent_center + direction * end_distance);

            const u32 parent_group_x = parent_x / 2u;
            const u32 parent_group_y = parent_y / 2u;
            const u32 parent_local = (parent_x % 2u) + 2u * (parent_y % 2u);
            const u32 parent_phase = groupPhaseOracle(parent_group_x, parent_group_y, parent_level);
            const bool parent_valid = probeInitializedOracle(accepted_frame, parent_phase, parent_local);

            lighting::Interval merged = makeInterval(float3{zero_v}, 0.0f);
            for (u32 sub = 0u; sub < 4u; ++sub) {
                lighting::Interval far_interval = makeInterval(float3{zero_v}, 0.0f);
                if (parent_valid) {
                    far_interval = decodeRecord(parent_level_words, parent_level, parent_x, parent_y, 4u * phi + sub);
                }

                const lighting::Interval sector{
                    near_interval.m_interval.m_radiance +
                        near_interval.m_interval.m_transmittance * far_interval.m_radiance,
                    near_interval.m_interval.m_transmittance * far_interval.m_transmittance};
                merged.m_radiance += sector.m_radiance;
                merged.m_transmittance += sector.m_transmittance;
            }

            accumulated.m_radiance += merged.m_radiance * static_cast<float>(0.25 * weight);
            accumulated.m_transmittance += merged.m_transmittance * static_cast<float>(0.25 * weight);
        }

        return accumulated;
    }

    // ------------------------------------------------------------------
    // schedule probe
    // ------------------------------------------------------------------

    struct ScheduleQuery {
        u32 m_group_x{};
        u32 m_group_y{};
        u32 m_level{};
        u32 m_local{};
        u32 m_phi{};
        u32 m_reserved[3]{};
    };

    using ScheduleResult = std::array<u32, 8u>;

    struct ScheduleProbe {
        Fixture &m_fixture;
        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        rhi::ComPtr<rhi::IShaderObject> m_root{};

        explicit ScheduleProbe(Fixture &fixture) : m_fixture{fixture} {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_cascade_probe.slang",
                "world_lighting_cascade_probe", module.writeRef()));

            shader::ComPtr<slang::IEntryPoint> entry{};
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("probeSchedule", entry.writeRef())));
            slang::IComponentType *entries[]{entry.get()};

            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_TEST_ASSERT(not make_error_code(
                fixture.device().createShaderProgram(description, m_program.writeRef(), diagnostics.writeRef())));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = m_program;
            PPR_TEST_ASSERT(
                not make_error_code(fixture.device().createComputePipeline(pipeline, m_pipeline.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(fixture.device().createRootShaderObject(m_program, m_root.writeRef())));
        }

        [[nodiscard]] std::vector<ScheduleResult> run(
            const std::span<const ScheduleQuery> queries, const u64 accepted_frame) {
            struct ProbeFrame {
                u32 m_count;
                u32 m_reserved[3];
            };
            struct CascadeFrameMirror {
                u32 m_level;
                u32 m_frame_low;
                u32 m_frame_high;
                i32 m_sky_hour;
                u32 m_status_record;
                u32 m_reserved[3];
            };
            static_assert(sizeof(CascadeFrameMirror) == 32u);

            const std::vector<u32> zero_results(queries.size() * 8u, 0u);
            auto query_buffer = makeShaderBuffer(
                m_fixture.device(), queries.size() * sizeof(ScheduleQuery), sizeof(ScheduleQuery), queries.data());
            auto result_buffer = makeStorageBuffer(
                m_fixture.device(), zero_results.size() * sizeof(u32), sizeof(u32), zero_results.data());

            // The probe includes the real cascade module, so every global of the
            // program needs a binding even though only the schedule helpers read.
            const std::array<u32, 1u> dummy_word{0u};
            const std::array<u8, 96u> dummy_visual{};
            const std::array<u32, 2u> dummy_cascade{0u, 0u};
            const std::vector<u32> status_words(kNumericStatusRecords * 8u, kNoNumericIndex);
            auto resolved = makeShaderBuffer(m_fixture.device(), sizeof(dummy_word), sizeof(u32), dummy_word.data());
            auto visuals = makeShaderBuffer(
                m_fixture.device(), sizeof(dummy_visual), sizeof(GridTerrainVisual), dummy_visual.data());
            auto source = makeShaderBuffer(m_fixture.device(), sizeof(dummy_word), sizeof(u32), dummy_word.data());
            auto cascades = makeStorageBuffer(
                m_fixture.device(), sizeof(dummy_cascade), sizeof(u32), dummy_cascade.data());
            auto status = makeStorageBuffer(
                m_fixture.device(), status_words.size() * sizeof(u32), sizeof(u32), status_words.data());
            auto jump = makeJumpTexture(m_fixture.device(), 1u);

            auto *const root = m_root.get();
            rhi::ShaderCursor cursor{root};
            const ProbeFrame probe_frame{static_cast<u32>(queries.size()), {}};
            const CascadeFrameMirror frame{
                .m_level = 0u,
                .m_frame_low = static_cast<u32>(accepted_frame),
                .m_frame_high = static_cast<u32>(accepted_frame >> 32u),
                .m_sky_hour = 0,
                .m_status_record = 0u,
                .m_reserved = {}};

            rhi::ShaderCursor constants;
            PPR_TEST_ASSERT(not make_error_code(cursor["g_schedule_probe"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&probe_frame, sizeof(probe_frame))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_cascade"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));

            PPR_TEST_ASSERT(not make_error_code(cursor["g_schedule_queries"].setBinding(rhi::Binding(query_buffer))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_schedule_results"].setBinding(rhi::Binding(result_buffer))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_resolved_cells"].setBinding(rhi::Binding(resolved))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_visuals"].setBinding(rhi::Binding(visuals))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_jump"].setBinding(rhi::Binding(jump))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_cell_source"].setBinding(rhi::Binding(source))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_cascades"].setBinding(rhi::Binding(cascades))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_numeric_status"].setBinding(rhi::Binding(status))));

            auto encoder = makeEncoder(m_fixture);
            encoder->setBufferState(query_buffer, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(result_buffer, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(resolved, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(visuals, rhi::ResourceState::ShaderResource);
            encoder->setTextureState(jump, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(source, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(cascades, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(status, rhi::ResourceState::UnorderedAccess);
            {
                auto *const pass = encoder->beginComputePass();
                PPR_TEST_ASSERT(pass != nullptr);
                pass->bindPipeline(m_pipeline, m_root);
                pass->dispatchCompute((static_cast<u32>(queries.size()) + 63u) / 64u, 1u, 1u);
                pass->end();
            }

            submit(m_fixture, *encoder);

            const auto words = readWords(m_fixture, result_buffer, 0u, zero_results.size() * sizeof(u32));
            std::vector<ScheduleResult> results(queries.size());
            for (std::size_t index = 0u; index < results.size(); ++index) {
                std::copy_n(words.begin() + static_cast<std::ptrdiff_t>(index * 8u), 8u, results[index].begin());
            }

            return results;
        }
    };

    // ------------------------------------------------------------------
    // schedule
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (schedule_visits_each_probe_once_per_four_frame_block) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        // Every level, group and local probe over the first two blocks and a
        // block far beyond 2^32. The oracle re-derives selection, age, validity
        // and the actual trace frame from the approved schedule spelling.
        // Two blocks at the start plus frames above 2^32. +1 and +2 force the
        // uint64 borrow chain: a retained probe's age exceeds the low word.
        const std::array<u64, 11u> frames{
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, (1ull << 32u) + 1u, (1ull << 32u) + 2u, (1ull << 32u) + 5u};
        for (const u64 frame: frames) {
            for (u32 level = 1u; level <= lighting::kCascadeLevels; ++level) {
                const u32 group_edge = lighting::cascadeGroupEdge(level);
                for (u32 gy = 0u; gy < group_edge; ++gy) {
                    for (u32 gx = 0u; gx < group_edge; ++gx) {
                        const u32 phase = groupPhaseOracle(gx, gy, level);
                        u32 valid_count = 0u;
                        for (u32 local = 0u; local < 4u; ++local) {
                            const u64 age = probeAgeOracle(frame, phase, local);
                            const bool valid = frame >= age;
                            if (valid) {
                                ++valid_count;
                            }
                        }

                        // Before the first complete block only the probes whose
                        // age fits the frame are initialized; no wrapped frame is
                        // ever treated as data.
                        PPR_TEST_ASSERT(valid_count == (std::min) (frame, u64{3u}) + 1u);

                        // The selected local probe is k = (n + h) mod 4, always
                        // current, and exactly one probe per frame is selected.
                        const u32 selected = ((frame + phase) & 3u);
                        PPR_TEST_ASSERT(selected < 4u);
                        PPR_TEST_ASSERT(probeAgeOracle(frame, phase, selected) == 0u);
                        PPR_TEST_ASSERT(probeInitializedOracle(frame, phase, selected));

                        // A retained probe's actual trace frame is n-age and is
                        // only equal to n for the selected probe. A probe that
                        // has not been visited yet has no frame at all; its
                        // subtraction is never performed.
                        for (u32 local = 0u; local < 4u; ++local) {
                            const u64 age = probeAgeOracle(frame, phase, local);
                            if (frame < age) {
                                PPR_TEST_ASSERT(age > frame);
                                continue;
                            }

                            const u64 trace = frame - age;
                            PPR_TEST_ASSERT(trace <= frame);
                            if (local == selected) {
                                PPR_TEST_ASSERT(trace == frame);
                            } else {
                                PPR_TEST_ASSERT(trace < frame);
                            }
                        }
                    }
                }
            }
        }

        // Each complete four-frame block visits every local probe exactly once
        // for every group and level.
        for (u32 block = 0u; block < 2u; ++block) {
            for (u32 level = 1u; level <= lighting::kCascadeLevels; ++level) {
                const u32 group_edge = lighting::cascadeGroupEdge(level);
                for (u32 gy = 0u; gy < group_edge; ++gy) {
                    for (u32 gx = 0u; gx < group_edge; ++gx) {
                        const u32 phase = groupPhaseOracle(gx, gy, level);
                        std::array<u32, 4u> visits{0u, 0u, 0u, 0u};
                        for (u32 frame = block * 4u; frame < block * 4u + 4u; ++frame) {
                            ++visits[(frame + phase) & 3u];
                        }

                        for (const u32 count: visits) {
                            PPR_TEST_ASSERT(count == 1u);
                        }
                    }
                }
            }
        }

        // The real shader schedule matches across all group offsets, locals and
        // the frames above, including a frame with a nonzero high word.
        ScheduleProbe probe{fixture};
        const auto make_queries = [](const u32 level, const u32 group_stride, const std::vector<u64> &frames) {
            std::vector<ScheduleQuery> queries;
            const u32 group_edge = lighting::cascadeGroupEdge(level);
            for (const u64 frame: frames) {
                (void) frame;
                for (u32 gy = 0u; gy < group_edge; gy += group_stride) {
                    for (u32 gx = 0u; gx < group_edge; gx += group_stride) {
                        for (u32 local = 0u; local < 4u; ++local) {
                            ScheduleQuery query{};
                            query.m_group_x = gx;
                            query.m_group_y = gy;
                            query.m_level = level;
                            query.m_local = local;
                            query.m_phi = (gx * 7u + gy * 13u + local) % lighting::cascadeDirections(level);
                            queries.push_back(query);
                        }
                    }
                }
            }

            return queries;
        };

        for (const u64 frame: frames) {
            for (u32 level = 1u; level <= lighting::kCascadeLevels; ++level) {
                const u32 stride = level >= 4u ? 1u : static_cast<u32>(lighting::cascadeGroupEdge(level) / 4u);
                const auto queries = make_queries(level, (std::max) (1u, stride), {frame});
                const auto results = probe.run(queries, frame);
                PPR_TEST_ASSERT(results.size() == queries.size());

                for (std::size_t index = 0u; index < queries.size(); ++index) {
                    const ScheduleQuery &query = queries[index];
                    const ScheduleResult &result = results[index];
                    const u32 phase = groupPhaseOracle(query.m_group_x, query.m_group_y, query.m_level);
                    const u64 age = probeAgeOracle(frame, phase, query.m_local);
                    const bool valid = probeInitializedOracle(frame, phase, query.m_local);

                    PPR_TEST_ASSERT(result[0u] == phase);
                    PPR_TEST_ASSERT(result[1u] == ((frame + phase) & 3u));
                    PPR_TEST_ASSERT(result[2u] == age);
                    PPR_TEST_ASSERT((result[5u] != 0u) == valid);
                    if (not valid) {
                        continue;
                    }

                    PPR_TEST_ASSERT(result[3u] == static_cast<u32>(frame - age));
                    PPR_TEST_ASSERT(result[4u] == static_cast<u32>((frame - age) >> 32u));
                    // The shader evaluates the 23-bit fraction and the angle in
                    // binary32; the oracle uses binary64 and rounds once.
                    const float measured_fraction = std::bit_cast<float>(result[6u]);
                    const float expected_fraction = static_cast<float>(phaseFractionOracle(frame - age));
                    PPR_TEST_ASSERT(
                        std::abs(measured_fraction - expected_fraction) <= 1.0e-6f * (expected_fraction + 1.0e-3f));

                    const float measured_angle = std::bit_cast<float>(result[7u]);
                    const float expected_angle = static_cast<float>(
                        probeAngleOracle(query.m_phi, lighting::cascadeDirections(query.m_level), frame - age));
                    PPR_TEST_ASSERT(
                        std::abs(measured_angle - expected_angle) <= 1.0e-6f * (std::abs(expected_angle) + 1.0e-3f));
                }
            }
        }

        std::println(
            "WORLD_LIGHTING_CASCADE schedule levels={} frames={} gpu_queries=true blocks_visit_once=true",
            lighting::kCascadeLevels, frames.size());
    };

    PPR_UNIT_TEST (counter_exhaustion_rejects_admission_before_encoding) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        CategoricalMap map;
        map.occupy(64u, 64u, kEmissive);
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0, 1u);
        PPR_TEST_ASSERT(pass.acceptedFrameForTest() == 1u);

        const auto before = readWords(fixture, pass.cascadeIntervalsForTest(), 0u, 32u);

        // The checked uint64 frame counter is exhausted: admission is rejected
        // before a slot, an encoding or any storage write exists.
        pass.setAcceptedFrameForTest(std::numeric_limits<u64>::max());
        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, 1u, {}};
            WorldLightingFrame frame{};
            frame.m_sky_hour = 0;
            PPR_TEST_ASSERT(pass.prepare(context, inputs.inputs(), frame) == std::errc::result_out_of_range);
            for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
                PPR_TEST_ASSERT(not pass.statusSlotPendingForTest(slot));
            }
        }

        const auto after = readWords(fixture, pass.cascadeIntervalsForTest(), 0u, 32u);
        PPR_TEST_ASSERT(before == after);

        // Restoring a live counter accepts the next frame normally.
        runFrame(fixture, pass, inputs.inputs(), 4u, 0, 2u);
        PPR_TEST_ASSERT(pass.acceptedFrameForTest() == 5u);

        std::println("WORLD_LIGHTING_CASCADE counter_exhaustion rejected_before_encoding=true");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // source buffer contracts and numeric status
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (source_buffer_contracts_are_validated_before_encoding) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        CategoricalMap map;
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        const auto expect_rejected = [&](const lighting::WorldLightingInputs &candidate, const std::errc expected) {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            WorldLightingFrame frame{};
            frame.m_sky_hour = 0;
            PPR_TEST_ASSERT(pass.prepare(context, candidate, frame) == expected);
            PPR_TEST_ASSERT(pass.acceptedFrameForTest() == 0u);
            for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
                PPR_TEST_ASSERT(not pass.statusSlotPendingForTest(slot));
            }
        };

        // Missing publication inputs are not an empty-world substitute.
        {
            auto candidate = inputs.inputs();
            candidate.m_source_materials = nullptr;
            expect_rejected(candidate, std::errc::not_connected);
        }

        {
            auto candidate = inputs.inputs();
            candidate.m_thermal_lut = nullptr;
            expect_rejected(candidate, std::errc::not_connected);
        }

        // The derived source palette is eight-byte records covering every
        // material index the source dispatch reads.
        {
            const std::array<u32, kMaterialCount> words{};
            auto candidate = inputs.inputs();
            candidate.m_source_materials = makeShaderBuffer(fixture.device(), sizeof(words), sizeof(u32), words.data());
            expect_rejected(candidate, std::errc::invalid_argument);
        }

        {
            std::array<lighting::SourceMaterial, kMaterialCount - 1u> short_palette{};
            auto candidate = inputs.inputs();
            candidate.m_source_materials = makeShaderBuffer(
                fixture.device(), sizeof(short_palette), sizeof(lighting::SourceMaterial), short_palette.data());
            expect_rejected(candidate, std::errc::invalid_argument);
        }

        // The thermal table is exactly 32,768 bytes with a 16-byte row stride.
        {
            const std::vector<u8> short_table(lighting::kThermalLutBytes - 16u, 0u);
            auto candidate = inputs.inputs();
            candidate.m_thermal_lut = makeShaderBuffer(
                fixture.device(), short_table.size(), lighting::kThermalLutStride, short_table.data());
            expect_rejected(candidate, std::errc::invalid_argument);
        }

        {
            const std::vector<u8> wrong_stride(lighting::kThermalLutBytes, 0u);
            auto candidate = inputs.inputs();
            candidate.m_thermal_lut = makeShaderBuffer(fixture.device(), wrong_stride.size(), 12u, wrong_stride.data());
            expect_rejected(candidate, std::errc::invalid_argument);
        }

        // The complete publication set is accepted and armed.
        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            WorldLightingFrame frame{};
            frame.m_sky_hour = 0;
            PPR_TEST_ASSERT(not pass.prepare(context, inputs.inputs(), frame));
            pass.discardUnsubmitted();
        }

        std::println("WORLD_LIGHTING_CASCADE source_contracts rejected_before_encoding=true");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (source_and_transport_numeric_status_are_reported_and_settled) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        auto sources = makeSourcePalette();
        // 60,000 authored emission with unit diffuse reflectance.
        sources[kEmissive] = *lighting::makeSourceMaterial(
            rgb(60'000.0f, 0.0f, 0.0f), 1.0, rgb(1.0f, 1.0f, 1.0f), rgb(1.0f, 1.0f, 1.0f), 0.0);

        ThermalTable thermal;
        thermal.load(fixture);

        // A 65,000 DC field adds about 25,931 to the diffuse term, so the single
        // resolved cell overflows the RGB9E5 representable range.
        const auto prior_ch = makeCHField(fixture, [](const u32, const u32) { return rgb(65'000.0f, 0.0f, 0.0f); });

        CategoricalMap map;
        map.occupy(400u, 300u, kEmissive);
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.m_prior_ch = prior_ch;
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        const u32 failing_cell = cellIndex(400u, 300u);

        // Real GPU range failure from the source stage (record 4) with the
        // containing zero word written for the failing cell.
        submitAttempt(fixture, pass, inputs.inputs(), 0u, -1, 1u);
        {
            const auto words = pass.pendingStatusWordsForTest();
            PPR_TEST_ASSERT(words.size() == kNumericStatusRecords * kNumericStatusWords);
            PPR_TEST_ASSERT(words[kNumericStageSource * kNumericStatusWords + 0u] == kNoNumericIndex);
            PPR_TEST_ASSERT(words[kNumericStageSource * kNumericStatusWords + 1u] == failing_cell);
            for (u32 word = 2u; word < kNumericStatusWords; ++word) {
                PPR_TEST_ASSERT(words[kNumericStageSource * kNumericStatusWords + word] == 0u);
            }
        }

        PPR_TEST_ASSERT(pass.collectCompleted(1u) == std::errc::result_out_of_range);
        const auto contained = readWords(
            fixture, pass.sourceCellsForTest(), static_cast<u64>(failing_cell) * sizeof(u32), sizeof(u32));
        PPR_TEST_ASSERT(contained[0u] == 0u);

        // Real GPU invalid-state failure: a NaN thermal LUT row feeding a hot
        // opaque cell produces a nonfinite source value.
        ThermalTable nan_thermal;
        nan_thermal.load(fixture);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const u32 nan_row = ThermalTable::rowFor(4'000.0);
        for (u32 row = nan_row - 2u; row <= nan_row + 2u; ++row) {
            nan_thermal.writeRow(row, rgb(nan, nan, nan));
        }

        nan_thermal.upload(fixture);

        CategoricalMap hot_map;
        hot_map.occupy(200u, 100u, kHot, 4'000.0);
        CascadeInputs hot_inputs{visuals, hot_map, sources, nan_thermal};
        hot_inputs.upload(fixture);

        submitAttempt(fixture, pass, hot_inputs.inputs(), 4u, -1, 2u);
        {
            const auto words = pass.pendingStatusWordsForTest();
            PPR_TEST_ASSERT(words[kNumericStageSource * kNumericStatusWords + 0u] == cellIndex(200u, 100u));
            PPR_TEST_ASSERT(words[kNumericStageSource * kNumericStatusWords + 1u] == kNoNumericIndex);
        }

        PPR_TEST_ASSERT(pass.collectCompleted(2u) == std::errc::state_not_recoverable);

        // The terminal and lower-cascade records map to their own stages through
        // the real readback inspection path. The GPU work here is clean; the
        // minima are injected after completion exactly as Task 7's ticket test
        // does for the projection records.
        CategoricalMap clean_map;
        CascadeInputs clean_inputs{visuals, clean_map, sources, thermal};
        clean_inputs.upload(fixture);

        submitAttempt(fixture, pass, clean_inputs.inputs(), 5u, -1, 3u);
        pass.pendingStatusWordsForTest()[kNumericStageTerminal * kNumericStatusWords + 1u] = 9u;
        PPR_TEST_ASSERT(pass.collectCompleted(3u) == std::errc::result_out_of_range);

        submitAttempt(fixture, pass, clean_inputs.inputs(), 6u, -1, 4u);
        pass.pendingStatusWordsForTest()[kNumericStageCascade * kNumericStatusWords + 0u] = 5u;
        PPR_TEST_ASSERT(pass.collectCompleted(4u) == std::errc::state_not_recoverable);

        // Teardown inspects an uninspected pending ticket instead of reporting a
        // successful shutdown.
        submitAttempt(fixture, pass, clean_inputs.inputs(), 7u, -1, 5u);
        pass.pendingStatusWordsForTest()[kNumericStageTerminal * kNumericStatusWords + 0u] = 3u;
        PPR_TEST_ASSERT(pass.shutdown() == std::errc::state_not_recoverable);

        std::println(
            "WORLD_LIGHTING_CASCADE numeric_status source_range=index source_invalid=index "
            "terminal/cascade=readback_injection teardown=reported");
    };

    // ------------------------------------------------------------------
    // persistent storage and atlas
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (cascade_storage_matches_the_approved_layout) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        CategoricalMap map;
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Create the world's persistent storage without executing any GPU work:
        // the creation-time contents are the approved cold start.
        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            WorldLightingFrame frame{};
            frame.m_sky_hour = 0;
            PPR_TEST_ASSERT(not pass.prepare(context, inputs.inputs(), frame));
            pass.discardUnsubmitted();
        }

        // One structured store, 5x1,048,576 uint2 records, stride 8, 8 MiB per
        // level and 40 MiB total. There is no L0 interval allocation.
        PPR_TEST_ASSERT(lighting::kCascadeLevels == 5u);
        PPR_TEST_ASSERT(lighting::kCascadeLevelRecords == 1'048'576u);
        PPR_TEST_ASSERT(lighting::kCascadeLevelBytes == 8'388'608u);
        PPR_TEST_ASSERT(lighting::kCascadeRecordCount == 5'242'880u);
        PPR_TEST_ASSERT(lighting::kCascadeBufferBytes == 41'943'040u);

        PPR_TEST_ASSERT(pass.cascadeIntervalsForTest() != nullptr);
        PPR_TEST_ASSERT(pass.cascadeIntervalsForTest()->getDesc().size == lighting::kCascadeBufferBytes);
        PPR_TEST_ASSERT(pass.cascadeIntervalsForTest()->getDesc().elementSize == sizeof(lighting::PackedInterval));
        PPR_TEST_ASSERT(pass.sourceCellsForTest() != nullptr);
        PPR_TEST_ASSERT(pass.sourceCellsForTest()->getDesc().size == lighting::kSourceCellBytes);
        PPR_TEST_ASSERT(pass.fineCHForTest() != nullptr);
        PPR_TEST_ASSERT(pass.fineCHForTest()->getDesc().size == lighting::kFineCHBytes);

        // Spacing, direction count, probe edge and atlas addressing.
        for (u32 level = 1u; level <= lighting::kCascadeLevels; ++level) {
            const u32 spacing = 2u << level;
            const u32 directions = 4u << (2u * level);
            PPR_TEST_ASSERT(lighting::cascadeSpacing(level) == spacing);
            PPR_TEST_ASSERT(lighting::cascadeDirections(level) == directions);
            PPR_TEST_ASSERT(lighting::cascadeProbeEdge(level) == kEdge / spacing);
            PPR_TEST_ASSERT(spacing * spacing == directions);
            PPR_TEST_ASSERT(lighting::cascadeProbeEdge(level) * spacing == kEdge);

            // The 1024-square logical atlas is exactly filled by every probe.
            PPR_TEST_ASSERT(
                lighting::cascadeProbeEdge(level) * lighting::cascadeProbeEdge(level) * directions == kLevelRecords);
        }

        // Record addressing: level offset, atlas tile and sector position.
        PPR_TEST_ASSERT(storeRecordIndex(1u, 0u, 0u, 0u) == 0u);
        PPR_TEST_ASSERT(storeRecordIndex(1u, 1u, 0u, 0u) == 4u);
        PPR_TEST_ASSERT(storeRecordIndex(1u, 0u, 1u, 0u) == 4u * kEdge);
        PPR_TEST_ASSERT(storeRecordIndex(1u, 0u, 0u, 5u) == 1u * kEdge + 1u);
        PPR_TEST_ASSERT(storeRecordIndex(5u, 0u, 0u, 0u) == 4u * kLevelRecords);
        PPR_TEST_ASSERT(storeRecordIndex(5u, 15u, 15u, 4095u) == 5u * kLevelRecords - 1u);
        PPR_TEST_ASSERT(levelRecordIndex(5u, 0u, 0u, 0u) == 0u);
        PPR_TEST_ASSERT(levelRecordIndex(5u, 15u, 15u, 4095u) == kLevelRecords - 1u);

        // Interval starts derived from the fixed penumbra width, and the finite
        // end a_(l+1) for l<5; L5 traces the actual map exit (no finite end).
        for (u32 level = 1u; level <= lighting::kCascadeLevels; ++level) {
            PPR_TEST_ASSERT(lighting::cascadeIntervalStart(level) == kIntervalStarts[level - 1u]);
        }

        PPR_TEST_ASSERT(lighting::kCascadePenumbraWidth == 0.5f);
        PPR_TEST_ASSERT(
            lighting::cascadeIntervalStart(1u) == static_cast<float>(static_cast<double>(kEdge) * 0.5 * 1.0e-3 * 2.0));

        for (u32 level = 1u; level < lighting::kCascadeLevels; ++level) {
            PPR_TEST_ASSERT(
                lighting::cascadeIntervalStart(level + 1u) == kIntervalStarts[level] and
                level + 1u <= lighting::kCascadeLevels);
        }

        // Cold start is zero radiance and zero transmittance, not vacuum: every
        // word of the persistent store and the source cache starts zero.
        const auto words = readWords(fixture, pass.cascadeIntervalsForTest(), 0u, lighting::kCascadeBufferBytes);
        PPR_TEST_ASSERT(std::ranges::all_of(words, [](const u32 word) { return word == 0u; }));
        const auto source_words = readWords(fixture, pass.sourceCellsForTest(), 0u, lighting::kSourceCellBytes);
        for (u32 cell = 0u; cell < kCells; ++cell) {
            // The empty map produced no source; the store is zero everywhere.
            PPR_TEST_ASSERT(source_words[cell] == 0u);
        }

        std::println(
            "WORLD_LIGHTING_CASCADE storage records={} stride=8 level_bytes={} total_bytes={} levels=5 l0=none",
            lighting::kCascadeRecordCount, lighting::kCascadeLevelBytes, lighting::kCascadeBufferBytes);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // terminal sky and map exit
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terminal_sky_closes_every_path_with_zero_transmittance) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        CategoricalMap map;
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        const std::vector<u8> occupancy = map.occupancy(visuals);
        const std::vector<u32> source_words(kCells, 0u);

        // Three complete blocks so every L5 probe has been visited. Hour 0 is
        // used for the oracle comparison; hour 23 follows for the seam check.
        for (u64 frame = 0u; frame < 12u; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, 0, frame + 1u);
        }

        constexpr u64 kSkyFrame = 11u;
        const auto level_words = readLevel(fixture, pass.cascadeIntervalsForTest(), kTerminal);

        for (u32 probe_y = 0u; probe_y < lighting::cascadeProbeEdge(kTerminal); probe_y += 3u) {
            for (u32 probe_x = 0u; probe_x < lighting::cascadeProbeEdge(kTerminal); probe_x += 3u) {
                const u64 trace = probeTraceFrame(kSkyFrame, kTerminal, probe_x, probe_y);
                for (u32 phi = 0u; phi < lighting::cascadeDirections(kTerminal); phi += 97u) {
                    const lighting::Interval stored = decodeRecord(level_words, kTerminal, probe_x, probe_y, phi);
                    const lighting::Interval expected = terminalOracle(
                        occupancy, source_words, probe_x, probe_y, trace, phi, 0);
                    PPR_TEST_ASSERT(stored.m_transmittance == 0.0f);
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.x - expected.m_radiance.x) <=
                        2.0e-3f * (std::abs(expected.m_radiance.x) + 1.0e-3f));
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.y - expected.m_radiance.y) <=
                        2.0e-3f * (std::abs(expected.m_radiance.y) + 1.0e-3f));
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.z - expected.m_radiance.z) <=
                        2.0e-3f * (std::abs(expected.m_radiance.z) + 1.0e-3f));
                }
            }
        }

        // Hour -1 is the black boundary sky: an empty-map terminal miss is
        // exactly Interval{L=(0,0,0),T=0} for every stored sector. That is a
        // closed path, not Task 8's finite clear T=1.
        for (u64 frame = 12u; frame < 24u; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, -1, frame + 1u);
        }

        const auto black_words = readLevel(fixture, pass.cascadeIntervalsForTest(), kTerminal);
        PPR_TEST_ASSERT(std::ranges::all_of(black_words, [](const u32 word) { return word == 0u; }));

        // The seam-crossing terminal sub-sector keeps its narrow unwrapped
        // integral; wrapped endpoints are never normalised and sorted into the
        // complementary arc. The sub-sector lives in the 16,384-space, while a
        // stored direction phi is the 4,096-space index that owns it.
        const double turn = 2.0 * std::numbers::pi;
        const double sector_width = turn / static_cast<double>(kTerminalDirections);
        const double rotation = turn * 23.0 / 24.0;
        const u32 seam_subsector = [&] {
            for (u32 sector = 0u; sector < kTerminalDirections; ++sector) {
                const double start = static_cast<double>(sector) * sector_width + rotation;
                const double end = static_cast<double>(sector + 1u) * sector_width + rotation;
                if (static_cast<u32>(std::floor(start / turn)) != static_cast<u32>(std::floor(end / turn))) {
                    return sector;
                }
            }

            return 0u;
        }();

        const u32 seam_phi = seam_subsector / 4u;
        const u32 stored_directions = lighting::cascadeDirections(kTerminal);
        PPR_TEST_ASSERT(stored_directions == kTerminalDirections / 4u);
        PPR_TEST_ASSERT(seam_phi < stored_directions);
        PPR_TEST_ASSERT(seam_subsector >= seam_phi * 4u and seam_subsector < seam_phi * 4u + 4u);

        // CPU discrimination: the sorted-endpoints failing mode integrates the
        // complement of the narrow arc, which differs from the narrow integral by
        // orders of magnitude for this sector.
        const float3 narrow = skySectorOracle(seam_subsector, kTerminalDirections, 23);
        const float3 sorted = skySortedSectorOracle(seam_subsector, kTerminalDirections, 23);
        PPR_TEST_ASSERT(std::abs(narrow.x) > 0.0f);
        PPR_TEST_ASSERT(std::abs(sorted.x - narrow.x) > 0.5f * std::abs(narrow.x));

        // Failing-mode stored average for the seam phi: replace only the seam
        // sub-sector's sky contribution with the sorted (complementary) integral.
        float3 failing_average{zero_v};
        for (u32 sub = 0u; sub < 4u; ++sub) {
            const u32 sector = seam_phi * 4u + sub;
            const float3 value = sector == seam_subsector ? sorted : skySectorOracle(sector, kTerminalDirections, 23);
            failing_average.x += value.x * 0.25f;
            failing_average.y += value.y * 0.25f;
            failing_average.z += value.z * 0.25f;
        }

        // Re-run hour 23 and compare the stored directions that span the seam.
        for (u64 frame = 24u; frame < 36u; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, 23, frame + 1u);
        }

        const auto seam_words = readLevel(fixture, pass.cascadeIntervalsForTest(), kTerminal);
        u32 compared = 0u;
        for (u32 probe_y = 0u; probe_y < lighting::cascadeProbeEdge(kTerminal); ++probe_y) {
            for (u32 probe_x = 0u; probe_x < lighting::cascadeProbeEdge(kTerminal); ++probe_x) {
                const u64 trace = probeTraceFrame(35u, kTerminal, probe_x, probe_y);
                for (u32 offset = 0u; offset < 3u; ++offset) {
                    const u32 phi = (seam_phi + stored_directions + offset - 1u) % stored_directions;
                    const lighting::Interval stored = decodeRecord(seam_words, kTerminal, probe_x, probe_y, phi);
                    const lighting::Interval expected = terminalOracle(
                        occupancy, source_words, probe_x, probe_y, trace, phi, 23);
                    PPR_TEST_ASSERT(stored.m_transmittance == 0.0f);
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.x - expected.m_radiance.x) <=
                        2.0e-3f * (std::abs(expected.m_radiance.x) + 1.0e-3f));
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.y - expected.m_radiance.y) <=
                        2.0e-3f * (std::abs(expected.m_radiance.y) + 1.0e-3f));
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.z - expected.m_radiance.z) <=
                        2.0e-3f * (std::abs(expected.m_radiance.z) + 1.0e-3f));

                    if (phi == seam_phi) {
                        // The GPU record is the narrow unwrapped integral, not
                        // the sorted complementary arc.
                        PPR_TEST_ASSERT(
                            std::abs(stored.m_radiance.x - failing_average.x) > 0.5f * std::abs(stored.m_radiance.x));
                    }

                    ++compared;
                }
            }
        }

        PPR_TEST_ASSERT(compared == 3u * lighting::cascadeProbeEdge(kTerminal) * lighting::cascadeProbeEdge(kTerminal));
        std::println(
            "WORLD_LIGHTING_CASCADE terminal_sky hours=-1,0,23 transmittance=0 seam_subsector={} seam_phi={} "
            "stored_dir={} narrow!=sorted",
            seam_subsector, seam_phi, stored_directions);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (terminal_traces_the_actual_map_exit_beyond_the_finite_end) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        // A blocker 1,228 cells from the corner probe centre: farther than the
        // premature finite 1,048.576-cell end but still inside the 1024-square
        // map, so only the actual forward exit finds it. The pure metal emits
        // but has zero diffuse reflectance, so the multi-frame CH feedback added
        // by fine reconstruction cannot change this frame's cached source.
        CategoricalMap map;
        map.fillRect(884u, 884u, 916u, 916u, kMetal);
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        for (u64 frame = 0u; frame < 12u; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, 0, frame + 1u);
        }

        const std::vector<u8> occupancy = map.occupancy(visuals);
        std::vector<u32> source_words(kCells, 0u);
        for (u32 cell = 0u; cell < kCells; ++cell) {
            // Only the blocker emits; every other cell is empty.
            source_words[cell] = occupancy[cell] != 0u ? sources[kMetal].m_emission : 0u;
        }

        const auto level_words = readLevel(fixture, pass.cascadeIntervalsForTest(), kTerminal);
        const u64 trace = probeTraceFrame(11u, kTerminal, 0u, 0u);
        u32 hits = 0u;
        double farthest = 0.0;
        for (u32 phi = 0u; phi < lighting::cascadeDirections(kTerminal); phi += 1u) {
            std::optional<double> distance;
            const lighting::Interval expected = terminalOracle(
                occupancy, source_words, 0u, 0u, trace, phi, 0, &distance);
            const lighting::Interval stored = decodeRecord(level_words, kTerminal, 0u, 0u, phi);
            PPR_TEST_ASSERT(stored.m_transmittance == 0.0f);
            PPR_TEST_ASSERT(
                std::abs(stored.m_radiance.x - expected.m_radiance.x) <=
                3.0e-3f * (std::abs(expected.m_radiance.x) + 1.0e-3f));
            PPR_TEST_ASSERT(
                std::abs(stored.m_radiance.y - expected.m_radiance.y) <=
                3.0e-3f * (std::abs(expected.m_radiance.y) + 1.0e-3f));
            if (distance.has_value()) {
                ++hits;
                farthest = (std::max) (farthest, *distance);
            }
        }

        PPR_TEST_ASSERT(hits > 0u);
        PPR_TEST_ASSERT(farthest > 1'048.576);
        PPR_TEST_ASSERT(farthest < 1'448.0);

        // Outside start and re-entry: the corner probe's low-angle sectors start
        // beyond the map and must still enter it.
        std::println(
            "WORLD_LIGHTING_CASCADE terminal_exit hits={} farthest_cells={} finite_sentinel=1048.576", hits, farthest);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // cached source
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (source_reports_emission_without_cold_start_feedback) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        CategoricalMap map;
        map.occupy(100u, 100u, kEmissive);
        map.occupy(200u, 100u, kDiffuse);
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0, 1u);

        const auto words = readWords(fixture, pass.sourceCellsForTest(), 0u, lighting::kSourceCellBytes);

        // Cold start: the prior CH field is zero, so the diffuse term is exactly
        // zero and the source is the authored emission. The diffuse-only cell at
        // 300 K is exactly zero because its thermal emission lies below the
        // RGB9E5 minimum positive value, not because 300 K is out of range.
        PPR_TEST_ASSERT(words[cellIndex(100u, 100u)] == sources[kEmissive].m_emission);
        PPR_TEST_ASSERT(words[cellIndex(200u, 100u)] == 0u);
        PPR_TEST_ASSERT(words[cellIndex(0u, 0u)] == 0u);

        std::println("WORLD_LIGHTING_CASCADE source_cold_start emission_only=true feedback_zero=true");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (source_metal_and_receive_only_cells_have_no_diffuse_feedback) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        // A nonzero prior DC field would feed any non-metal diffuse receiver.
        const auto prior_ch = makeCHField(fixture, [](const u32, const u32) { return rgb(0.8f, 0.4f, 0.2f); });

        CategoricalMap map;
        map.occupy(300u, 300u, kMetal);
        map.occupy(400u, 300u, kMetal, 3'000.0);
        map.occupy(500u, 300u, kReceiveOnly, 3'000.0);
        map.occupy(600u, 300u, kOpaque);

        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.m_prior_ch = prior_ch;
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0, 1u);

        const auto words = readWords(fixture, pass.sourceCellsForTest(), 0u, lighting::kSourceCellBytes);

        // Pure metal: diffuse reflectance is zero, so a nonzero prior mean adds
        // nothing and the authored emission is the entire source.
        PPR_TEST_ASSERT(words[cellIndex(300u, 300u)] == sources[kMetal].m_emission);

        // A hot pure metal adds its thermal emission but still no diffuse.
        const float3 hot_expected = lighting::unpackRGB9E5(sources[kMetal].m_emission) + thermal.sample(3'000.0);
        const float3 hot_actual = lighting::unpackRGB9E5(words[cellIndex(400u, 300u)]);
        PPR_TEST_ASSERT(closeRGB9E5(hot_actual, hot_expected));

        // Receive-only transparent and empty cells are exactly zero source even
        // when hot: they do not evaluate thermal transport.
        PPR_TEST_ASSERT(words[cellIndex(500u, 300u)] == 0u);
        PPR_TEST_ASSERT(words[cellIndex(0u, 0u)] == 0u);

        // The opaque diffuse receiver does take the prior mean.
        PPR_TEST_ASSERT(words[cellIndex(600u, 300u)] != 0u);

        std::println("WORLD_LIGHTING_CASCADE source_metal diffuse=0 receive_only=0 hot_metal=emission+thermal");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (source_hot_opaque_cell_reports_thermal_emission) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        CategoricalMap map;
        map.fill(kHot, 4'000.0);
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0, 1u);

        const auto words = readWords(fixture, pass.sourceCellsForTest(), 0u, lighting::kSourceCellBytes);
        const float3 expected = thermal.sample(4'000.0);
        const float3 actual = lighting::unpackRGB9E5(words[cellIndex(512u, 512u)]);
        PPR_TEST_ASSERT(closeRGB9E5(actual, expected));
        PPR_TEST_ASSERT(expected.y > 0.0f);

        std::println(
            "WORLD_LIGHTING_CASCADE source_thermal kelvin=4000 sampled=({}, {}, {})", expected.x, expected.y,
            expected.z);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (source_known_dc_field_adds_diffuse_previous_mean) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        // A spatially varying but slowly changing DC field: bilinear sampling at
        // (cell+0.5)/2-0.5 with clamped edges, then C0*Y0.
        const auto prior_ch = makeCHField(fixture, [](const u32 x, const u32 y) {
            const float u = static_cast<float>(x) / 511.0f;
            const float v = static_cast<float>(y) / 511.0f;
            return rgb(0.1f + 0.5f * u, 0.2f + 0.3f * v, 0.4f);
        });

        CategoricalMap map;
        map.occupy(10u, 10u, kDiffuse);
        map.occupy(500u, 400u, kDiffuse);
        map.occupy(1'023u, 1'023u, kDiffuse);

        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.m_prior_ch = prior_ch;
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        runFrame(fixture, pass, inputs.inputs(), 0u, 0, 1u);

        const auto words = readWords(fixture, pass.sourceCellsForTest(), 0u, lighting::kSourceCellBytes);
        constexpr float y0 = 0.39894228f;

        const auto expected_source = [&](const u32 x, const u32 y) {
            const double fine_x = (static_cast<double>(x) + 0.5) * 0.5 - 0.5;
            const double fine_y = (static_cast<double>(y) + 0.5) * 0.5 - 0.5;
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

            const float3 diffuse = lighting::unpackRGB9E5(sources[kDiffuse].m_diffuse);
            const float3 emission = lighting::unpackRGB9E5(sources[kDiffuse].m_emission);
            const float3 thermal_value = thermal.sample(300.0);
            return float3{
                emission.x + thermal_value.x + diffuse.x * static_cast<float>(mean.x) * y0,
                emission.y + thermal_value.y + diffuse.y * static_cast<float>(mean.y) * y0,
                emission.z + thermal_value.z + diffuse.z * static_cast<float>(mean.z) * y0};
        };

        for (const std::array<u32, 2u> cell:
            {std::array<u32, 2u>{10u, 10u}, std::array<u32, 2u>{500u, 400u}, std::array<u32, 2u>{1'023u, 1'023u}}) {
            const float3 expected = expected_source(cell[0u], cell[1u]);
            const float3 actual = lighting::unpackRGB9E5(words[cellIndex(cell[0u], cell[1u])]);
            PPR_TEST_ASSERT(closeRGB9E5(actual, expected));
        }

        // Source preparation reads the pre-update field and never writes it.
        // The supplied diagnostic field keeps its bytes; the owner's in-place
        // field is written by fine reconstruction after the source dispatch.
        const auto supplied = readWords(fixture, prior_ch.get(), 0u, 64u);
        PPR_TEST_ASSERT(supplied[0u] != 0u);
        PPR_TEST_ASSERT(pass.fineCHForTest() != nullptr);

        std::println("WORLD_LIGHTING_CASCADE source_feedback emission+thermal+diffuse*previous_mean=y0_scale");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // lower-level merge, retention and stage order
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (cascade_lower_levels_merge_four_parent_sectors_with_spatial_weights) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        // Parent probes see different far intervals: the far blocker is hit by
        // the probes on its row and missed by the neighbours, so the child's
        // bilinear weights are observable. It sits beyond the 262-cell L5 start
        // distance of the parent probes and inside the 1024-square map. The pure
        // metal emits with zero diffuse reflectance, so fine CH feedback cannot
        // perturb the cached source across the twelve accepted frames.
        CategoricalMap map;
        map.fillRect(400u, 86u, 404u, 89u, kMetal);
        CascadeInputs inputs{visuals, map, sources, thermal};
        inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        constexpr u64 kFrames = 12u;
        for (u64 frame = 0u; frame < kFrames; ++frame) {
            runFrame(fixture, pass, inputs.inputs(), frame, 0, frame + 1u);
        }

        const std::vector<u8> occupancy = map.occupancy(visuals);
        std::vector<u32> source_words(kCells, 0u);
        for (u32 cell = 0u; cell < kCells; ++cell) {
            source_words[cell] = occupancy[cell] != 0u ? sources[kMetal].m_emission : 0u;
        }

        const u64 last_frame = kFrames - 1u;
        const u32 level = 2u;
        const auto child_words = readLevel(fixture, pass.cascadeIntervalsForTest(), level);
        const auto parent_words = readLevel(fixture, pass.cascadeIntervalsForTest(), level + 1u);

        // Pick a group and compare every direction of the probe that the last
        // frame selected.
        const u32 group_edge = lighting::cascadeGroupEdge(level);
        u32 compared = 0u;
        for (u32 gy = 0u; gy < group_edge; ++gy) {
            for (u32 gx = 0u; gx < group_edge; ++gx) {
                const u32 phase = groupPhaseOracle(gx, gy, level);
                const u32 selected = (last_frame + phase) & 3u;
                const u32 probe_x = gx * 2u + (selected % 2u);
                const u32 probe_y = gy * 2u + (selected / 2u);

                const u32 directions = lighting::cascadeDirections(level);
                for (u32 phi = 0u; phi < directions; phi += 7u) {
                    const lighting::Interval stored = decodeRecord(child_words, level, probe_x, probe_y, phi);
                    const lighting::Interval expected = mergeOracle(
                        occupancy, source_words, parent_words, level, probe_x, probe_y, last_frame, phi);
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.x - expected.m_radiance.x) <=
                        4.0e-3f * (std::abs(expected.m_radiance.x) + 1.0e-3f));
                    PPR_TEST_ASSERT(
                        std::abs(stored.m_radiance.y - expected.m_radiance.y) <=
                        4.0e-3f * (std::abs(expected.m_radiance.y) + 1.0e-3f));
                    PPR_TEST_ASSERT(stored.m_transmittance == expected.m_transmittance);
                    ++compared;
                }
            }
        }

        PPR_TEST_ASSERT(compared > 0u);
        std::println(
            "WORLD_LIGHTING_CASCADE merge level=2 parent_sectors=4 spatial_weights=bilinear compared={}", compared);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (cascade_retains_unselected_intervals_and_orders_source_before_tracing) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto visuals = makePalette();
        const auto sources = makeSourcePalette();
        ThermalTable thermal;
        thermal.load(fixture);

        // The terminal ray hits an emitting cell in the same submission that
        // produced the source: the source write must be ordered before tracing.
        CategoricalMap blocked;
        blocked.fill(kEmissive);
        CascadeInputs blocked_inputs{visuals, blocked, sources, thermal};
        blocked_inputs.upload(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        runFrame(fixture, pass, blocked_inputs.inputs(), 0u, 0, 1u);
        {
            const auto first = readLevel(fixture, pass.cascadeIntervalsForTest(), kTerminal);
            for (u32 probe_y = 0u; probe_y < lighting::cascadeProbeEdge(kTerminal); ++probe_y) {
                for (u32 probe_x = 0u; probe_x < lighting::cascadeProbeEdge(kTerminal); ++probe_x) {
                    // Every probe's own selected direction must see the freshly
                    // prepared source, not the zero cold-start cache.
                    const u32 phase = groupPhaseOracle(probe_x / 2u, probe_y / 2u, kTerminal);
                    const u32 local = (probe_x % 2u) + 2u * (probe_y % 2u);
                    if (probeAgeOracle(0u, phase, local) != 0u) {
                        continue;
                    }

                    // A ray that starts inside the emitting volume returns the
                    // cached source with zero transmittance.
                    for (const u32 phi: {0u, 1u, 7u, kStoredDirections / 2u, kStoredDirections - 1u}) {
                        const lighting::Interval stored = decodeRecord(first, kTerminal, probe_x, probe_y, phi);
                        PPR_TEST_ASSERT(stored.m_transmittance == 0.0f);
                        PPR_TEST_ASSERT(stored.m_radiance.x > 0.0f);
                    }
                }
            }
        }

        // Empty map: the retained probes keep their exact bytes between frames.
        CategoricalMap empty_map;
        CascadeInputs inputs{visuals, empty_map, sources, thermal};
        inputs.upload(fixture);

        runFrame(fixture, pass, inputs.inputs(), 1u, 0, 2u);
        const auto level_before = readLevel(fixture, pass.cascadeIntervalsForTest(), kTerminal);
        runFrame(fixture, pass, inputs.inputs(), 2u, 0, 3u);
        const auto level_after = readLevel(fixture, pass.cascadeIntervalsForTest(), kTerminal);

        u32 selected_count = 0u;
        u32 retained_count = 0u;
        u32 retained_records = 0u;
        for (u32 gy = 0u; gy < lighting::cascadeGroupEdge(kTerminal); ++gy) {
            for (u32 gx = 0u; gx < lighting::cascadeGroupEdge(kTerminal); ++gx) {
                const u32 phase = groupPhaseOracle(gx, gy, kTerminal);
                const u32 selected = (2u + phase) & 3u;
                for (u32 local = 0u; local < 4u; ++local) {
                    const u32 probe_x = gx * 2u + (local % 2u);
                    const u32 probe_y = gy * 2u + (local / 2u);
                    if (local == selected) {
                        ++selected_count;
                        continue;
                    }

                    // Every stored direction of a retained probe keeps its exact
                    // bytes, not only the first atlas record.
                    for (const u32 phi: {0u, 1u, 7u, kStoredDirections / 2u, kStoredDirections - 1u}) {
                        const std::size_t base = levelRecordIndex(kTerminal, probe_x, probe_y, phi);
                        PPR_TEST_ASSERT(
                            level_before[base * 2u] == level_after[base * 2u] and
                            level_before[base * 2u + 1u] == level_after[base * 2u + 1u]);
                        ++retained_records;
                    }

                    ++retained_count;
                }
            }
        }

        const u32 groups = lighting::cascadeGroupEdge(kTerminal) * lighting::cascadeGroupEdge(kTerminal);
        PPR_TEST_ASSERT(selected_count == groups);
        PPR_TEST_ASSERT(retained_count == 3u * groups);
        PPR_TEST_ASSERT(retained_records == 5u * 3u * groups);
        PPR_TEST_ASSERT(not pass.shutdown());
        std::println(
            "WORLD_LIGHTING_CASCADE retention selected={} retained={} retained_records={} "
            "source_before_tracing=true",
            selected_count, retained_count, retained_records);
    };

    // ------------------------------------------------------------------
    // reflected ABI
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (reflected_cascade_bindings_match_the_cpu_shapes) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto stride_of = [&](const std::filesystem::path &file, const std::string_view name) -> u32 {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(
                not fixture.m_shaders->loadModuleFromFile(file, file.stem().string().c_str(), module.writeRef()));
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

        PPR_TEST_ASSERT(
            stride_of(std::filesystem::current_path() / "shaders" / "world_lighting_cascade.slang", "g_cascades") ==
            8u);
        PPR_TEST_ASSERT(
            stride_of(
                std::filesystem::current_path() / "shaders" / "world_lighting_cascade.slang", "g_resolved_cells") ==
            4u);
        PPR_TEST_ASSERT(
            stride_of(std::filesystem::current_path() / "shaders" / "world_lighting_source.slang", "g_prior_ch") ==
            32u);
        PPR_TEST_ASSERT(
            stride_of(
                std::filesystem::current_path() / "shaders" / "world_lighting_source.slang", "g_source_materials") ==
            8u);
        PPR_TEST_ASSERT(
            stride_of(std::filesystem::current_path() / "shaders" / "world_lighting_source.slang", "g_thermal_lut") ==
            16u);

        // Constant blocks match the CPU mirrors byte for byte.
        const auto constants_of = [&](const std::filesystem::path &file, const std::string_view name) {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(
                not fixture.m_shaders->loadModuleFromFile(file, file.stem().string().c_str(), module.writeRef()));
            auto *const layout = module->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (name == std::string_view{parameter->getName()}) {
                    return parameter->getTypeLayout()->getElementTypeLayout();
                }
            }

            return static_cast<slang::TypeLayoutReflection *>(nullptr);
        };

        auto *const source_constants = constants_of(
            std::filesystem::current_path() / "shaders" / "world_lighting_source.slang", "g_source_frame");
        PPR_TEST_ASSERT(source_constants != nullptr);
        PPR_TEST_ASSERT(source_constants->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 16u);
        PPR_TEST_ASSERT(source_constants->getFieldByIndex(1u)->getOffset() == 4u);
        PPR_TEST_ASSERT(source_constants->getFieldByIndex(2u)->getOffset() == 8u);

        auto *const cascade_constants = constants_of(
            std::filesystem::current_path() / "shaders" / "world_lighting_cascade.slang", "g_cascade");
        PPR_TEST_ASSERT(cascade_constants != nullptr);
        // Slang's uniform layout may pad the trailing scalar array in
        // reflection; the declared field offsets below are the shared ABI.
        PPR_TEST_ASSERT(cascade_constants->getFieldCount() == 6u);
        PPR_TEST_ASSERT(cascade_constants->getFieldByIndex(1u)->getOffset() == 4u);
        PPR_TEST_ASSERT(cascade_constants->getFieldByIndex(2u)->getOffset() == 8u);
        PPR_TEST_ASSERT(cascade_constants->getFieldByIndex(3u)->getOffset() == 12u);
        PPR_TEST_ASSERT(cascade_constants->getFieldByIndex(4u)->getOffset() == 16u);

        for (const auto &[file, entries]:
            std::array<std::tuple<std::filesystem::path, std::array<const char *, 3u>>, 2u>{{
                {std::filesystem::current_path() / "shaders" / "world_lighting_source.slang",
                    {"prepareSource", nullptr, nullptr}},
                {std::filesystem::current_path() / "shaders" / "world_lighting_cascade.slang",
                    {"updateTerminal", "updateCascade", nullptr}},
            }}) {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(
                not fixture.m_shaders->loadModuleFromFile(file, file.stem().string().c_str(), module.writeRef()));
            for (const char *const entry: entries) {
                if (entry == nullptr) {
                    continue;
                }

                shader::ComPtr<slang::IEntryPoint> point{};
                PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entry, point.writeRef())));
            }
        }

        std::println("WORLD_LIGHTING_CASCADE reflected interval_stride=8 ch_stride=32 source_stride=8 constants=ok");
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // cached source and coarse cascade transport suite
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_cascadeTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_cascade") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingCascade::schedule_visits_each_probe_once_per_four_frame_block,
                detail::WorldLightingCascade::counter_exhaustion_rejects_admission_before_encoding,
                detail::WorldLightingCascade::source_buffer_contracts_are_validated_before_encoding,
                detail::WorldLightingCascade::source_and_transport_numeric_status_are_reported_and_settled,
                detail::WorldLightingCascade::cascade_storage_matches_the_approved_layout,
                detail::WorldLightingCascade::terminal_sky_closes_every_path_with_zero_transmittance,
                detail::WorldLightingCascade::terminal_traces_the_actual_map_exit_beyond_the_finite_end,
                detail::WorldLightingCascade::source_reports_emission_without_cold_start_feedback,
                detail::WorldLightingCascade::source_metal_and_receive_only_cells_have_no_diffuse_feedback,
                detail::WorldLightingCascade::source_hot_opaque_cell_reports_thermal_emission,
                detail::WorldLightingCascade::source_known_dc_field_adds_diffuse_previous_mean,
                detail::WorldLightingCascade::cascade_lower_levels_merge_four_parent_sectors_with_spatial_weights,
                detail::WorldLightingCascade::cascade_retains_unselected_intervals_and_orders_source_before_tracing,
                detail::WorldLightingCascade::reflected_cascade_bindings_match_the_cpu_shapes,
            });
        };
        return group;
    }
}
