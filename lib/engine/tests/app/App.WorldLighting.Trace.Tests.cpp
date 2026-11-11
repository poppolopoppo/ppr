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

namespace pP::tests::detail::WorldLightingTrace {
    // ------------------------------------------------------------------
    // fixture values
    // ------------------------------------------------------------------

    inline constexpr u32 kEdge = WorldLightingPass::kWorldEdge;
    inline constexpr u32 kCells = WorldLightingPass::kCellCount;
    inline constexpr u32 kMaterialCount = 8u;

    inline constexpr u32 kOpaque = 1u;
    inline constexpr u32 kReceiveOnly = 2u;
    inline constexpr u32 kBrightReceiveOnly = 3u;
    inline constexpr u32 kEmissiveOpaque = 4u;
    inline constexpr u32 kOpaqueBlack = 5u;

    inline constexpr u32 kVisibilityDispatchGroups = kEdge / 32u;
    inline constexpr u32 kJumpDispatchGroups = kEdge / 16u;

    static_assert(WorldLightingPass::kVisibilityRecordCount == 21'504u);
    static_assert(WorldLightingPass::kVisibilityBufferBytes == 172'032u);
    static_assert(lighting::kOpticalBlendSurfaceFlag == kGridSurfaceOpticalBlend);

    constexpr u32 cellIndex(const u32 x, const u32 y) noexcept {
        return y * kEdge + x;
    }

    constexpr std::array<u32, 7u> kJumpValues{0u, 1u, 2u, 4u, 8u, 16u, 32u};

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
                "WORLD_LIGHTING_TRACE api={} adapter={} backend={}", info.apiName != nullptr ? info.apiName : "unknown",
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

    // ------------------------------------------------------------------
    // categorical map and palette fixtures
    // ------------------------------------------------------------------

    struct CategoricalMap {
        std::vector<u32> m_words{};

        CategoricalMap() {
            m_words.assign(kCells, 0u);
        }

        void occupy(const u32 x, const u32 y, const u32 material = kOpaque) {
            PPR_TEST_ASSERT(x < kEdge and y < kEdge);
            const auto word = lighting::packCell(static_cast<u16>(material), 300.0);
            PPR_TEST_ASSERT(word.has_value());
            m_words[cellIndex(x, y)] = *word;
        }

        void fill(const u32 material) {
            const auto word = lighting::packCell(static_cast<u16>(material), 300.0);
            PPR_TEST_ASSERT(word.has_value());
            m_words.assign(kCells, *word);
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
        }

        // Material 2 is transparent (receive-only) with a dark colour.
        palette[kReceiveOnly].m_surface_flags = kGridSurfaceOpticalBlend;
        palette[kReceiveOnly].m_color_rgba = 0x0000'00ffu;
        palette[kReceiveOnly].m_opacity = 0.5f;
        // Material 3 is a bright but still receive-only surface: occupancy must
        // not follow brightness.
        palette[kBrightReceiveOnly].m_surface_flags = kGridSurfaceOpticalBlend;
        palette[kBrightReceiveOnly].m_color_rgba = 0xffff'ffffu;
        palette[kBrightReceiveOnly].m_opacity = 0.5f;
        // Material 4 is an opaque black surface that emits: still occupied.
        palette[kEmissiveOpaque].m_color_rgba = 0x0000'00ffu;
        palette[kEmissiveOpaque].m_emissive_rgb[0] = 1.0f;
        palette[kEmissiveOpaque].m_emissive_strength = 5.0f;
        // Material 5 is opaque black with no emission.
        palette[kOpaqueBlack].m_color_rgba = 0x0000'00ffu;
        return palette;
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> uploadPalette(
        Fixture &fixture, const std::array<GridTerrainVisual, kMaterialCount> &palette) {
        return makeShaderBuffer(fixture.device(), sizeof(palette), sizeof(GridTerrainVisual), palette.data());
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> uploadTerrain(Fixture &fixture, const CategoricalMap &map) {
        return makeShaderBuffer(fixture.device(), map.m_words.size() * sizeof(u32), sizeof(u32), map.m_words.data());
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> uploadSource(Fixture &fixture, const u32 source_word) {
        const std::vector<u32> words(kCells, source_word);
        return makeShaderBuffer(fixture.device(), words.size() * sizeof(u32), sizeof(u32), words.data());
    }

    // ------------------------------------------------------------------
    // acceleration attempt driving
    // ------------------------------------------------------------------

    struct AccelerationSnapshot {
        rhi::IBuffer *m_visibility{};
        rhi::ITexture *m_jump{};
    };

    struct AccelerationAttempt {
        std::error_code m_prepare{};
        std::error_code m_finalize{};
        AccelerationSnapshot m_snapshot{};
    };

    // Encodes one projection + acceleration attempt into its own submission.
    // The caller owns acceptance (notifySubmitted) and settlement.
    [[nodiscard]] AccelerationAttempt encodeAcceleration(
        Fixture &fixture, WorldLightingPass &pass, const rhi::ComPtr<rhi::IBuffer> &palette, const CategoricalMap &map,
        const FenceCompletionValue completed = kFenceValueNone, const bool wait = true) {
        const auto terrain = uploadTerrain(fixture, map);

        lighting::WorldLightingInputs inputs{};
        inputs.m_terrain = terrain;
        inputs.m_visible_materials = palette;
        inputs.m_material_count = kMaterialCount;

        AccelerationAttempt attempt{};
        auto encoder = makeEncoder(fixture);
        PreRenderContext context{fixture.device(), *encoder, completed, {}};
        attempt.m_prepare = pass.prepare(context, inputs, WorldLightingFrame{});
        if (attempt.m_prepare) {
            return attempt;
        }

        attempt.m_snapshot.m_visibility = pass.accelerationVisibilityForTest();
        attempt.m_snapshot.m_jump = pass.accelerationJumpForTest();
        attempt.m_finalize = pass.finalize(context);
        submit(fixture, *encoder, wait);
        return attempt;
    }

    [[nodiscard]] std::vector<u32> readVisibilityRecords(Fixture &fixture, rhi::IBuffer *const buffer) {
        PPR_TEST_ASSERT(buffer != nullptr);
        PPR_TEST_ASSERT(buffer->getDesc().size == WorldLightingPass::kVisibilityBufferBytes);
        PPR_TEST_ASSERT(buffer->getDesc().elementSize == sizeof(lighting::VisibilityRecord));

        std::vector<u32> words(WorldLightingPass::kVisibilityRecordCount * 2u);
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().readBuffer(buffer, 0u, words.size() * sizeof(u32), words.data())));
        return words;
    }

    [[nodiscard]] std::vector<u8> readJumpTable(Fixture &fixture, rhi::ITexture *const texture) {
        PPR_TEST_ASSERT(texture != nullptr);
        PPR_TEST_ASSERT(texture->getDesc().format == rhi::Format::R8Uint);
        PPR_TEST_ASSERT(texture->getDesc().size.width == kEdge and texture->getDesc().size.height == kEdge);
        PPR_TEST_ASSERT(texture->getDesc().mipCount == 1u and texture->getDesc().sampleCount == 1u);

        shader::ComPtr<ISlangBlob> blob{};
        rhi::SubresourceLayout layout{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readTexture(texture, 0u, 0u, blob.writeRef(), &layout)));
        PPR_TEST_ASSERT(blob.get() != nullptr and blob->getBufferPointer() != nullptr);

        const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());
        std::vector<u8> jumps(static_cast<std::size_t>(kEdge) * kEdge);
        for (u32 y = 0u; y < kEdge; ++y) {
            std::memcpy(
                jumps.data() + static_cast<std::size_t>(y) * kEdge,
                source + static_cast<std::size_t>(y) * layout.rowPitch, kEdge);
        }

        return jumps;
    }

    [[nodiscard]] u32 readMaskWord(
        const std::vector<u32> &records, const u32 level, const u32 record_x, const u32 record_y, const bool high) {
        const u32 index = lighting::visibilityRecordIndex(level, record_x, record_y);
        return records[static_cast<std::size_t>(index) * 2u + (high ? 1u : 0u)];
    }

    // ------------------------------------------------------------------
    // exact CPU occupancy references
    // ------------------------------------------------------------------

    // OR of the exact native 1x1/2x2/4x4 footprint of one record bit.
    [[nodiscard]] u32 expectedMask(
        const std::vector<u8> &occupancy, const u32 level, const u32 record_x, const u32 record_y, const bool high) {
        const u32 footprint = 1u << level;
        const u32 record_extent = 8u * footprint;

        u32 result = 0u;
        for (u32 bit_y = 0u; bit_y < 8u; ++bit_y) {
            for (u32 bit_x = 0u; bit_x < 8u; ++bit_x) {
                bool occupied = false;
                for (u32 sample_y = 0u; sample_y < footprint and not occupied; ++sample_y) {
                    for (u32 sample_x = 0u; sample_x < footprint and not occupied; ++sample_x) {
                        const u32 x = record_x * record_extent + bit_x * footprint + sample_x;
                        const u32 y = record_y * record_extent + bit_y * footprint + sample_y;
                        occupied = occupancy[cellIndex(x, y)] != 0u;
                    }
                }

                if (not occupied) {
                    continue;
                }

                const u32 bit = bit_x + 8u * bit_y;
                if ((bit < 32u) != high) {
                    result |= 1u << (bit & 31u);
                }
            }
        }

        return result;
    }

    struct SummedAreaTable {
        std::vector<u32> m_sums{};

        explicit SummedAreaTable(const std::vector<u8> &occupancy) {
            const u32 side = kEdge + 1u;
            m_sums.assign(static_cast<std::size_t>(side) * side, 0u);
            for (u32 y = 0u; y < kEdge; ++y) {
                for (u32 x = 0u; x < kEdge; ++x) {
                    const u32 above = m_sums[static_cast<std::size_t>(y) * side + (x + 1u)];
                    const u32 left = m_sums[static_cast<std::size_t>(y + 1u) * side + x];
                    const u32 diagonal = m_sums[static_cast<std::size_t>(y) * side + x];
                    m_sums[static_cast<std::size_t>(y + 1u) * side + (x + 1u)] =
                        above + left - diagonal + (occupancy[cellIndex(x, y)] != 0u ? 1u : 0u);
                }
            }
        }

        [[nodiscard]] u32 sum(const u32 x0, const u32 y0, const u32 extent) const {
            const u32 side = kEdge + 1u;
            const u32 x1 = x0 + extent;
            const u32 y1 = y0 + extent;
            return m_sums[static_cast<std::size_t>(y1) * side + x1] -
                   m_sums[static_cast<std::size_t>(y0) * side + x1] -
                   m_sums[static_cast<std::size_t>(y1) * side + x0] +
                   m_sums[static_cast<std::size_t>(y0) * side + x0];
        }
    };

    // Largest empty aligned square containing the cell, or 0 when occupied.
    [[nodiscard]] u32 expectedJump(
        const SummedAreaTable &sums, const std::vector<u8> &occupancy, const u32 x, const u32 y) {
        if (occupancy[cellIndex(x, y)] != 0u) {
            return 0u;
        }

        for (const u32 side: {32u, 16u, 8u, 4u, 2u, 1u}) {
            const u32 base_x = (x / side) * side;
            const u32 base_y = (y / side) * side;
            if (sums.sum(base_x, base_y, side) == 0u) {
                return side;
            }
        }

        return 0u;
    }

    // ------------------------------------------------------------------
    // trace probe
    // ------------------------------------------------------------------

    struct TraceResult {
        std::vector<u32> m_words{};
        std::vector<u32> m_status{};
    };

    struct TraceProbe {
        Fixture &m_fixture;
        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        rhi::ComPtr<rhi::IShaderObject> m_root{};

        explicit TraceProbe(Fixture &fixture) : m_fixture{fixture} {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_trace_probe.slang",
                "world_lighting_trace_probe", module.writeRef()));

            shader::ComPtr<slang::IEntryPoint> entry{};
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("probeSegments", entry.writeRef())));
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

        [[nodiscard]] TraceResult run(
            rhi::IBuffer *const resolved, rhi::IBuffer *const palette, rhi::ITexture *const jump,
            rhi::IBuffer *const source, const std::span<const float4> segments) {
            struct ProbeFrame {
                u32 m_count;
                u32 m_reserved[3];
            };

            const std::array<u32, 32u> initial_status = [] {
                std::array<u32, 32u> status{};
                for (u32 record = 0u; record < 4u; ++record) {
                    status[record * 8u] = kNoNumericIndex;
                    status[record * 8u + 1u] = kNoNumericIndex;
                }
                return status;
            }();

            auto status = makeStorageBuffer(
                m_fixture.device(), initial_status.size() * sizeof(u32), sizeof(u32), initial_status.data());
            auto segment_buffer = makeShaderBuffer(
                m_fixture.device(), segments.size() * sizeof(float4), sizeof(float4), segments.data());
            const std::vector<u32> zero_output(segments.size() * 5u, 0u);
            auto output = makeStorageBuffer(
                m_fixture.device(), zero_output.size() * sizeof(u32), sizeof(u32), zero_output.data());

            rhi::ShaderCursor cursor{m_root.get()};
            rhi::ShaderCursor constants;
            PPR_TEST_ASSERT(not make_error_code(cursor["g_trace_probe"].getDereferenced(constants)));
            const ProbeFrame frame{static_cast<u32>(segments.size()), {}};
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));
            PPR_TEST_ASSERT(not make_error_code(
                rhi::ShaderCursor{m_root.get()}["g_trace_segments"].setBinding(rhi::Binding(segment_buffer))));
            PPR_TEST_ASSERT(
                not make_error_code(rhi::ShaderCursor{m_root.get()}["g_trace_out"].setBinding(rhi::Binding(output))));
            PPR_TEST_ASSERT(not make_error_code(
                rhi::ShaderCursor{m_root.get()}["g_resolved_cells"].setBinding(rhi::Binding(resolved))));
            PPR_TEST_ASSERT(
                not make_error_code(rhi::ShaderCursor{m_root.get()}["g_visuals"].setBinding(rhi::Binding(palette))));
            PPR_TEST_ASSERT(
                not make_error_code(rhi::ShaderCursor{m_root.get()}["g_jump"].setBinding(rhi::Binding(jump))));
            PPR_TEST_ASSERT(
                not make_error_code(rhi::ShaderCursor{m_root.get()}["g_cell_source"].setBinding(rhi::Binding(source))));
            PPR_TEST_ASSERT(not make_error_code(
                rhi::ShaderCursor{m_root.get()}["g_numeric_status"].setBinding(rhi::Binding(status))));

            auto encoder = makeEncoder(m_fixture);
            encoder->setBufferState(resolved, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(palette, rhi::ResourceState::ShaderResource);
            encoder->setTextureState(jump, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(source, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(segment_buffer, rhi::ResourceState::ShaderResource);
            encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(status, rhi::ResourceState::UnorderedAccess);
            {
                auto *const pass = encoder->beginComputePass();
                PPR_TEST_ASSERT(pass != nullptr);
                pass->bindPipeline(m_pipeline, m_root);
                pass->dispatchCompute((static_cast<u32>(segments.size()) + 63u) / 64u, 1u, 1u);
                pass->end();
            }

            submit(m_fixture, *encoder);

            TraceResult result{};
            result.m_words.resize(zero_output.size());
            result.m_status.resize(initial_status.size());
            PPR_TEST_ASSERT(not make_error_code(
                m_fixture.device().readBuffer(output, 0u, result.m_words.size() * sizeof(u32), result.m_words.data())));
            PPR_TEST_ASSERT(not make_error_code(m_fixture.device().readBuffer(
                status, 0u, result.m_status.size() * sizeof(u32), result.m_status.data())));
            return result;
        }
    };

    [[nodiscard]] float4 makeSegment(const float2 start, const float2 end) noexcept {
        return float4{start.x, start.y, end.x, end.y};
    }

    struct CpuTrace {
        u32 m_hit{kNoNumericIndex};
        lighting::Interval m_interval{};
    };

    [[nodiscard]] CpuTrace referenceTrace(
        const std::vector<u8> &occupancy, const u32 source_word, const float4 segment) {
        CpuTrace result{};
        const auto hit = lighting::traceNativeCellDDA(
            std::span<const u8>{occupancy.data(), occupancy.size()}, kEdge, float2{segment.x, segment.y},
            float2{segment.z, segment.w});
        if (not hit) {
            result.m_interval.m_radiance = float3{zero_v};
            result.m_interval.m_transmittance = 1.0f;
            return result;
        }

        result.m_hit = *hit;
        result.m_interval.m_radiance = lighting::unpackRGB9E5(source_word);
        result.m_interval.m_transmittance = 0.0f;
        return result;
    }

    // ------------------------------------------------------------------
    // block and mask checks
    // ------------------------------------------------------------------

    void verifyMasks(const std::vector<u32> &records, const std::vector<u8> &occupancy, const char *const label) {
        for (u32 level = 0u; level < lighting::kVisibilityLevelPitches.size(); ++level) {
            const u32 records_per_edge = lighting::kVisibilityLevelPitches[level];
            for (u32 record_y = 0u; record_y < records_per_edge; ++record_y) {
                for (u32 record_x = 0u; record_x < records_per_edge; ++record_x) {
                    for (const bool high: {false, true}) {
                        const u32 measured = readMaskWord(records, level, record_x, record_y, high);
                        const u32 expected = expectedMask(occupancy, level, record_x, record_y, high);
                        if (measured != expected) {
                            std::println(
                                "WORLD_LIGHTING_TRACE mask_mismatch case={} level={} record=({}, {}) high={} "
                                "measured={:x} expected={:x}",
                                label, level, record_x, record_y, high, measured, expected);
                        }

                        PPR_TEST_ASSERT(measured == expected);
                    }
                }
            }
        }
    }

    void verifyJumps(
        const std::vector<u8> &jumps, const std::vector<u8> &occupancy, const SummedAreaTable &sums,
        const char *const label) {
        PPR_TEST_ASSERT(jumps.size() == static_cast<std::size_t>(kEdge) * kEdge);

        for (u32 y = 0u; y < kEdge; ++y) {
            for (u32 x = 0u; x < kEdge; ++x) {
                const u32 expected = expectedJump(sums, occupancy, x, y);
                const u32 measured = jumps[cellIndex(x, y)];
                if (measured != expected) {
                    std::println(
                        "WORLD_LIGHTING_TRACE jump_mismatch case={} cell=({}, {}) measured={} expected={}", label, x, y,
                        measured, expected);
                }

                PPR_TEST_ASSERT(measured == expected);
            }
        }

        // Every claimed multi-cell block is exhaustively empty.
        for (u32 y = 0u; y < kEdge; ++y) {
            for (u32 x = 0u; x < kEdge; ++x) {
                const u32 side = jumps[cellIndex(x, y)];
                if (side < 2u) {
                    continue;
                }

                const u32 base_x = (x / side) * side;
                const u32 base_y = (y / side) * side;
                PPR_TEST_ASSERT(sums.sum(base_x, base_y, side) == 0u);
            }
        }
    }

    // ------------------------------------------------------------------
    // accelerated versus native-cell oracle
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (accelerated_matches_native_cell_oracle) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto palette_values = makePalette();
        const auto palette = uploadPalette(fixture, palette_values);
        const auto source_word = *lighting::packRGB9E5(float3{0.5f, 0.25f, 2.0f});
        const auto source = uploadSource(fixture, source_word);
        TraceProbe probe{fixture};

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        const auto run_case = [&](const char *const label, const CategoricalMap &map,
                                  const std::vector<float4> &segments) {
            const auto occupancy = map.occupancy(palette_values);
            const auto attempt = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const auto result = probe.run(
                pass.resolvedCellsForTest(), palette.get(), attempt.m_snapshot.m_jump, source.get(), segments);

            u32 hits = 0u;
            for (u32 index = 0u; index < segments.size(); ++index) {
                const CpuTrace expected = referenceTrace(occupancy, source_word, segments[index]);
                const u32 base = index * 5u;
                if (result.m_words[base] != expected.m_hit or
                    std::bit_cast<float>(result.m_words[base + 4u]) != expected.m_interval.m_transmittance) {
                    std::println(
                        "WORLD_LIGHTING_TRACE oracle_mismatch case={} segment={} start=({}, {}) end=({}, {}) "
                        "measured_cell={} expected_cell={} measured_t={} expected_t={}",
                        label, index, static_cast<float>(segments[index].x), static_cast<float>(segments[index].y),
                        static_cast<float>(segments[index].z), static_cast<float>(segments[index].w),
                        result.m_words[base], expected.m_hit, std::bit_cast<float>(result.m_words[base + 4u]),
                        expected.m_interval.m_transmittance);
                }

                PPR_TEST_ASSERT(result.m_words[base] == expected.m_hit);
                PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 1u]) == expected.m_interval.m_radiance.x);
                PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 2u]) == expected.m_interval.m_radiance.y);
                PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 3u]) == expected.m_interval.m_radiance.z);
                PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 4u]) == expected.m_interval.m_transmittance);
                if (expected.m_hit != kNoNumericIndex) {
                    ++hits;
                }
            }

            PPR_TEST_ASSERT(result.m_status[0u] == kNoNumericIndex);
            PPR_TEST_ASSERT(result.m_status[1u] == kNoNumericIndex);
            std::println("WORLD_LIGHTING_TRACE oracle case={} segments={} hits={}", label, segments.size(), hits);
        };

        // Empty: every finite segment is clear.
        {
            CategoricalMap map;
            run_case(
                "empty", map,
                {
                    makeSegment(float2{0.5f, 0.5f}, float2{1023.5f, 0.5f}),
                    makeSegment(float2{1023.5f, 1023.5f}, float2{0.5f, 0.5f}),
                    makeSegment(float2{0.5f, 0.5f}, float2{1023.5f, 1023.5f}),
                    makeSegment(float2{0.5f, 512.5f}, float2{1023.5f, 512.5f}),
                });
        }

        // Full: every segment hits its entered start cell, including an occupied
        // start.
        {
            CategoricalMap map;
            map.fill(kOpaque);
            run_case(
                "full", map,
                {
                    makeSegment(float2{10.5f, 20.5f}, float2{900.5f, 20.5f}),
                    makeSegment(float2{900.5f, 700.5f}, float2{10.5f, 700.5f}),
                    makeSegment(float2{10.5f, 10.5f}, float2{900.5f, 900.5f}),
                    makeSegment(float2{400.25f, 400.75f}, float2{400.75f, 400.25f}),
                });
        }

        // A single occupied cell: axis, negative, diagonal and corner rays.
        {
            CategoricalMap map;
            map.occupy(500u, 500u);
            run_case(
                "single", map,
                {
                    makeSegment(float2{100.5f, 500.5f}, float2{900.5f, 500.5f}),
                    makeSegment(float2{900.5f, 500.5f}, float2{100.5f, 500.5f}),
                    makeSegment(float2{500.5f, 100.5f}, float2{500.5f, 900.5f}),
                    makeSegment(float2{500.5f, 900.5f}, float2{500.5f, 100.5f}),
                    makeSegment(float2{400.5f, 400.5f}, float2{600.5f, 600.5f}),
                    makeSegment(float2{600.5f, 400.5f}, float2{400.5f, 600.5f}),
                    makeSegment(float2{100.5f, 499.5f}, float2{900.5f, 499.5f}),
                    makeSegment(float2{499.5f, 100.5f}, float2{499.5f, 900.5f}),
                    makeSegment(float2{500.5f, 500.5f}, float2{500.9f, 500.9f}),
                });
        }

        // Chunk seams: cells straddling 128-cell chunk boundaries.
        {
            CategoricalMap map;
            map.occupy(127u, 127u);
            map.occupy(128u, 128u);
            map.occupy(255u, 0u);
            run_case(
                "seams", map,
                {
                    makeSegment(float2{100.5f, 127.5f}, float2{200.5f, 127.5f}),
                    makeSegment(float2{100.5f, 128.5f}, float2{200.5f, 128.5f}),
                    makeSegment(float2{127.5f, 100.5f}, float2{127.5f, 200.5f}),
                    makeSegment(float2{128.5f, 100.5f}, float2{128.5f, 200.5f}),
                    makeSegment(float2{200.5f, 0.5f}, float2{300.5f, 0.5f}),
                    makeSegment(float2{127.0f, 127.0f}, float2{129.0f, 129.0f}),
                });
        }

        // Every jump size occurs around one occupied cell. Each segment crosses
        // cells whose jump is 1/2/4/8/16/32 and is compared to the oracle.
        {
            CategoricalMap map;
            map.occupy(0u, 0u);
            run_case(
                "jumps", map,
                {
                    makeSegment(float2{0.5f, 0.5f}, float2{40.5f, 0.5f}),
                    makeSegment(float2{0.5f, 1.5f}, float2{40.5f, 1.5f}),
                    makeSegment(float2{0.5f, 4.5f}, float2{40.5f, 4.5f}),
                    makeSegment(float2{0.5f, 8.5f}, float2{40.5f, 8.5f}),
                    makeSegment(float2{0.5f, 16.5f}, float2{40.5f, 16.5f}),
                    makeSegment(float2{0.5f, 32.5f}, float2{40.5f, 32.5f}),
                    makeSegment(float2{40.5f, 0.5f}, float2{0.5f, 0.5f}),
                    makeSegment(float2{40.5f, 40.5f}, float2{0.5f, 0.5f}),
                });
        }

        // Zero-length, sub-cell and point-only contacts.
        {
            CategoricalMap map;
            map.occupy(300u, 300u);
            map.occupy(700u, 700u);
            run_case(
                "degenerate", map,
                {
                    makeSegment(float2{300.5f, 300.5f}, float2{300.5f, 300.5f}),
                    makeSegment(float2{300.1f, 300.2f}, float2{300.9f, 300.8f}),
                    makeSegment(float2{299.1f, 299.2f}, float2{299.9f, 299.8f}),
                    makeSegment(float2{699.0f, 699.0f}, float2{700.0f, 700.0f}),
                    makeSegment(float2{700.0f, 700.0f}, float2{701.0f, 701.0f}),
                    makeSegment(float2{700.5f, 700.5f}, float2{700.5001f, 700.5001f}),
                });
        }

        // Outside starts entering the map, pointing away, and re-crossing a seam.
        {
            CategoricalMap map;
            map.occupy(10u, 512u);
            run_case(
                "outside", map,
                {
                    makeSegment(float2{-100.0f, 512.5f}, float2{100.0f, 512.5f}),
                    makeSegment(float2{-100.0f, 512.5f}, float2{-1.0f, 512.5f}),
                    makeSegment(float2{1100.0f, 512.5f}, float2{900.0f, 512.5f}),
                    makeSegment(float2{-100.0f, -100.0f}, float2{1100.0f, 612.5f}),
                    makeSegment(float2{512.5f, -50.0f}, float2{512.5f, 50.0f}),
                    makeSegment(float2{512.5f, 1100.0f}, float2{512.5f, 1000.0f}),
                    makeSegment(float2{-1.0f, 512.5f}, float2{11.0f, 512.5f}),
                });
        }

        // Opaque black source: a hit reports zero radiance and zero
        // transmittance, not the clear interval.
        {
            CategoricalMap map;
            map.occupy(512u, 512u);
            const auto occupancy = map.occupancy(palette_values);
            const auto black_source = uploadSource(fixture, 0u);

            const auto attempt = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const std::array<float4, 2u> segments{
                makeSegment(float2{512.5f, 512.5f}, float2{512.9f, 512.9f}),
                makeSegment(float2{100.5f, 100.5f}, float2{900.5f, 900.5f}),
            };
            const auto result = probe.run(
                pass.resolvedCellsForTest(), palette.get(), attempt.m_snapshot.m_jump, black_source.get(),
                std::span<const float4>{segments.data(), segments.size()});

            const CpuTrace expected = referenceTrace(occupancy, 0u, segments[0u]);
            PPR_TEST_ASSERT(result.m_words[0u] == expected.m_hit);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[1u]) == 0.0f);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[2u]) == 0.0f);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[3u]) == 0.0f);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[4u]) == 0.0f);

            // The diagonal reaches the same black cell.
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[9u]) == 0.0f);
            std::println("WORLD_LIGHTING_TRACE opaque_black radiance=(0,0,0) transmittance=0");
        }

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // visibility masks
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (visibility_masks_match_native_occupancy) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto palette_values = makePalette();
        const auto palette = uploadPalette(fixture, palette_values);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        const auto run_case = [&](const char *const label, const CategoricalMap &map) {
            const auto attempt = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const auto records = readVisibilityRecords(fixture, attempt.m_snapshot.m_visibility);
            const auto occupancy = map.occupancy(palette_values);
            verifyMasks(records, occupancy, label);
            std::println(
                "WORLD_LIGHTING_TRACE masks case={} records={} bytes={} occupied={}", label,
                WorldLightingPass::kVisibilityRecordCount, WorldLightingPass::kVisibilityBufferBytes,
                std::ranges::count_if(occupancy, [](const u8 value) { return value != 0u; }));
        };

        {
            CategoricalMap map;
            run_case("empty", map);
        }

        {
            CategoricalMap map;
            map.fill(kOpaque);
            run_case("full", map);
        }

        {
            CategoricalMap map;
            for (u32 y = 0u; y < kEdge; ++y) {
                for (u32 x = 0u; x < kEdge; ++x) {
                    if (((x + y) & 1u) == 0u) {
                        map.occupy(x, y);
                    }
                }
            }
            run_case("checkerboard", map);
        }

        {
            // Seeded sparse and dense maps exercise word, 8/16/32-tile and
            // world-edge boundaries including bits 31/32/63.
            CategoricalMap sparse;
            CategoricalMap dense;
            u32 state = 0x1234'5678u;
            const auto next = [&state] {
                state = state * 1'664'525u + 1'013'904'223u;
                return state >> 8u;
            };

            for (u32 cell = 0u; cell < kCells; ++cell) {
                const u32 value = next();
                if (value % 97u == 0u) {
                    sparse.occupy(cell % kEdge, cell / kEdge);
                }

                if (value % 3u == 0u) {
                    dense.occupy(cell % kEdge, cell / kEdge);
                }
            }

            run_case("sparse", sparse);
            run_case("dense", dense);
        }

        {
            // One occupied cell per 32x32 tile enumerates all 1024 local
            // positions across the 1024 tiles.
            CategoricalMap map;
            for (u32 tile = 0u; tile < 1024u; ++tile) {
                const u32 tile_x = tile % 32u;
                const u32 tile_y = tile / 32u;
                const u32 local_x = tile % 32u;
                const u32 local_y = (tile / 32u) % 32u;
                map.occupy(tile_x * 32u + local_x, tile_y * 32u + local_y);
            }

            const auto attempt = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const auto records = readVisibilityRecords(fixture, attempt.m_snapshot.m_visibility);
            const auto occupancy = map.occupancy(palette_values);
            verifyMasks(records, occupancy, "one_per_tile");

            // Explicit bit-position coverage in level 0.
            bool bit31 = false;
            bool bit32 = false;
            bool bit63 = false;
            for (u32 record = 0u; record < 128u * 128u; ++record) {
                if ((records[record * 2u] & (1u << 31u)) != 0u) {
                    bit31 = true;
                }
                if ((records[record * 2u + 1u] & 1u) != 0u) {
                    bit32 = true;
                }
                if ((records[record * 2u + 1u] & (1u << 31u)) != 0u) {
                    bit63 = true;
                }
            }

            PPR_TEST_ASSERT(bit31 and bit32 and bit63);
            std::println("WORLD_LIGHTING_TRACE mask_bits bit31={} bit32={} bit63={}", bit31, bit32, bit63);
        }

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // jump maximality and classification
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (jump_is_largest_aligned_empty_block) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto palette_values = makePalette();
        const auto palette = uploadPalette(fixture, palette_values);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        std::array<bool, 33u> seen{};
        const auto run_case = [&](const char *const label, const CategoricalMap &map) {
            const auto attempt = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const auto occupancy = map.occupancy(palette_values);
            const SummedAreaTable sums{occupancy};
            const auto jumps = readJumpTable(fixture, attempt.m_snapshot.m_jump);
            verifyJumps(jumps, occupancy, sums, label);
            for (const u8 value: jumps) {
                PPR_TEST_ASSERT(std::ranges::find(kJumpValues, static_cast<u32>(value)) != kJumpValues.end());
                seen[value] = true;
            }

            std::println(
                "WORLD_LIGHTING_TRACE jumps case={} occupied={} domain=[0,1,2,4,8,16,32]", label,
                std::ranges::count_if(occupancy, [](const u8 value) { return value != 0u; }));
        };

        {
            CategoricalMap map;
            run_case("empty", map);
        }

        {
            CategoricalMap map;
            map.occupy(0u, 0u);
            run_case("single_cell_all_sizes", map);
        }

        {
            CategoricalMap map;
            map.fill(kOpaque);
            run_case("full", map);
        }

        {
            // Opaque black is occupied, bright receive-only is transparent and
            // an emissive opaque surface stays occupied.
            CategoricalMap map;
            map.occupy(40u, 40u, kOpaqueBlack);
            map.occupy(100u, 40u, kBrightReceiveOnly);
            map.occupy(200u, 40u, kReceiveOnly);
            map.occupy(300u, 40u, kEmissiveOpaque);
            run_case("classification", map);

            const auto attempt = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const auto jumps = readJumpTable(fixture, attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(jumps[cellIndex(40u, 40u)] == 0u);
            PPR_TEST_ASSERT(jumps[cellIndex(300u, 40u)] == 0u);
            PPR_TEST_ASSERT(jumps[cellIndex(100u, 40u)] == 32u);
            PPR_TEST_ASSERT(jumps[cellIndex(200u, 40u)] == 32u);
        }

        {
            CategoricalMap map;
            u32 state = 0x0bad'c0deu;
            for (u32 cell = 0u; cell < kCells; ++cell) {
                state = state * 1'664'525u + 1'013'904'223u;
                if ((state >> 9u) % 41u == 0u) {
                    map.occupy(cell % kEdge, cell / kEdge);
                }
            }
            run_case("sparse_maximal", map);
        }

        for (const u32 value: kJumpValues) {
            PPR_TEST_ASSERT(seen[value]);
        }

        std::println("WORLD_LIGHTING_TRACE jump_domain all_seven=true");
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // reusable acceleration storage
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (acceleration_rebuild_drops_previous_occupancy) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto palette_values = makePalette();
        const auto palette = uploadPalette(fixture, palette_values);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // The first rebuild follows the creation-time nonzero poison, so a
        // missed record write cannot look like a valid empty domain.
        {
            CategoricalMap empty;
            const auto attempt = encodeAcceleration(fixture, pass, palette, empty);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const auto records = readVisibilityRecords(fixture, attempt.m_snapshot.m_visibility);
            PPR_TEST_ASSERT(std::ranges::all_of(records, [](const u32 word) { return word == 0u; }));
            const auto jumps = readJumpTable(fixture, attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(std::ranges::all_of(jumps, [](const u8 value) { return value == 32u; }));
        }

        {
            CategoricalMap full;
            full.fill(kOpaque);
            const auto attempt = encodeAcceleration(fixture, pass, palette, full);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));

            const auto records = readVisibilityRecords(fixture, attempt.m_snapshot.m_visibility);
            PPR_TEST_ASSERT(std::ranges::all_of(records, [](const u32 word) { return word == 0xffff'ffffu; }));
            const auto jumps = readJumpTable(fixture, attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(std::ranges::all_of(jumps, [](const u8 value) { return value == 0u; }));
        }

        // A moving single cell: the old site regains the exact legal larger
        // jump once its 32x32 aligned block is empty again.
        {
            CategoricalMap first;
            first.occupy(100u, 100u);
            auto attempt = encodeAcceleration(fixture, pass, palette, first);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));
            auto jumps = readJumpTable(fixture, attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(jumps[cellIndex(100u, 100u)] == 0u);

            CategoricalMap second;
            second.occupy(900u, 900u);
            attempt = encodeAcceleration(fixture, pass, palette, second);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));
            jumps = readJumpTable(fixture, attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(jumps[cellIndex(900u, 900u)] == 0u);
            PPR_TEST_ASSERT(jumps[cellIndex(100u, 100u)] == 32u);
        }

        // Checkerboard: occupied cells 0, their 2x2 neighbours 1.
        {
            CategoricalMap map;
            for (u32 y = 0u; y < kEdge; ++y) {
                for (u32 x = 0u; x < kEdge; ++x) {
                    if (((x + y) & 1u) == 0u) {
                        map.occupy(x, y);
                    }
                }
            }

            const auto attempt = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));
            const auto jumps = readJumpTable(fixture, attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(jumps[cellIndex(0u, 0u)] == 0u);
            PPR_TEST_ASSERT(jumps[cellIndex(1u, 0u)] == 1u);
            PPR_TEST_ASSERT(jumps[cellIndex(0u, 1u)] == 1u);
            PPR_TEST_ASSERT(jumps[cellIndex(1u, 1u)] == 0u);
        }

        // Queued successive submissions with no intervening CPU wait: each
        // completion-fenced snapshot matches its own input generation.
        {
            CategoricalMap full;
            full.fill(kOpaque);
            const auto full_attempt = encodeAcceleration(fixture, pass, palette, full, 0u, false);
            PPR_TEST_ASSERT(not full_attempt.m_prepare);
            PPR_TEST_ASSERT(not full_attempt.m_finalize);
            pass.notifySubmitted(1u);

            CategoricalMap empty;
            const auto empty_attempt = encodeAcceleration(fixture, pass, palette, empty, 0u, false);
            PPR_TEST_ASSERT(not empty_attempt.m_prepare);
            PPR_TEST_ASSERT(not empty_attempt.m_finalize);
            pass.notifySubmitted(2u);

            PPR_TEST_ASSERT(full_attempt.m_snapshot.m_visibility != empty_attempt.m_snapshot.m_visibility);
            PPR_TEST_ASSERT(full_attempt.m_snapshot.m_jump != empty_attempt.m_snapshot.m_jump);

            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

            const auto full_records = readVisibilityRecords(fixture, full_attempt.m_snapshot.m_visibility);
            PPR_TEST_ASSERT(std::ranges::all_of(full_records, [](const u32 word) { return word == 0xffff'ffffu; }));
            const auto full_jumps = readJumpTable(fixture, full_attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(std::ranges::all_of(full_jumps, [](const u8 value) { return value == 0u; }));

            const auto empty_records = readVisibilityRecords(fixture, empty_attempt.m_snapshot.m_visibility);
            PPR_TEST_ASSERT(std::ranges::all_of(empty_records, [](const u32 word) { return word == 0u; }));
            const auto empty_jumps = readJumpTable(fixture, empty_attempt.m_snapshot.m_jump);
            PPR_TEST_ASSERT(std::ranges::all_of(empty_jumps, [](const u8 value) { return value == 32u; }));

            PPR_TEST_ASSERT(not pass.collectCompleted(2u));
        }

        // Asymmetric one-cell-per-tile input repeated on the same allocations;
        // a shared-memory ordering error would not be idempotent.
        {
            CategoricalMap map;
            for (u32 tile = 0u; tile < 1024u; ++tile) {
                const u32 tile_x = tile % 32u;
                const u32 tile_y = tile / 32u;
                const u32 local_x = (tile / 7u) % 32u;
                const u32 local_y = (tile / 13u) % 32u;
                map.occupy(tile_x * 32u + local_x, tile_y * 32u + local_y);
            }

            const auto first = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not first.m_prepare);
            PPR_TEST_ASSERT(not first.m_finalize);
            pass.notifySubmitted(1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(1u));
            const auto first_jumps = readJumpTable(fixture, first.m_snapshot.m_jump);
            const auto first_masks = readVisibilityRecords(fixture, first.m_snapshot.m_visibility);

            const auto second = encodeAcceleration(fixture, pass, palette, map);
            PPR_TEST_ASSERT(not second.m_prepare);
            PPR_TEST_ASSERT(not second.m_finalize);
            pass.notifySubmitted(2u);
            PPR_TEST_ASSERT(not pass.collectCompleted(2u));
            const auto second_jumps = readJumpTable(fixture, second.m_snapshot.m_jump);
            const auto second_masks = readVisibilityRecords(fixture, second.m_snapshot.m_visibility);

            PPR_TEST_ASSERT(first_jumps == second_jumps);
            PPR_TEST_ASSERT(first_masks == second_masks);

            const auto occupancy = map.occupancy(palette_values);
            const SummedAreaTable sums{occupancy};
            verifyJumps(second_jumps, occupancy, sums, "one_per_tile_repeat");
        }

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // reflected shape and allocation contract
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (reflected_visibility_records_match_cpu_shapes) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "world_lighting_visibility.slang",
            "world_lighting_visibility", module.writeRef()));

        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        const auto element = [&](const std::string_view name) -> slang::TypeLayoutReflection * {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (name == std::string_view{parameter->getName()}) {
                    return parameter->getTypeLayout()->getElementTypeLayout();
                }
            }

            return nullptr;
        };

        for (const auto &[name, stride]: std::array<std::pair<const char *, u32>, 3u>{{
                 {"g_visibility", 8u},
                 {"g_resolved_cells", 4u},
                 {"g_visuals", 96u},
             }}) {
            auto *const type = element(name);
            PPR_TEST_ASSERT(type != nullptr);
            const u32 measured = safe_narrowing(type->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM));
            std::println("WORLD_LIGHTING_TRACE reflected name={} stride={} expected={}", name, measured, stride);
            PPR_TEST_ASSERT(measured == stride);
        }

        for (const char *const entry: {"buildVisibility", "buildJump"}) {
            shader::ComPtr<slang::IEntryPoint> point{};
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entry, point.writeRef())));
        }

        // The real pass allocates one 21,504-record structured buffer and one
        // 1024-square R8_UINT jump texel per attempt slot.
        const auto palette_values = makePalette();
        const auto palette = uploadPalette(fixture, palette_values);
        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        CategoricalMap map;
        const auto attempt = encodeAcceleration(fixture, pass, palette, map);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        rhi::IBuffer *const visibility = attempt.m_snapshot.m_visibility;
        PPR_TEST_ASSERT(visibility != nullptr);
        PPR_TEST_ASSERT(visibility->getDesc().elementSize == 8u);
        PPR_TEST_ASSERT(visibility->getDesc().size == 172'032u);

        rhi::ITexture *const jump = attempt.m_snapshot.m_jump;
        PPR_TEST_ASSERT(jump != nullptr);
        PPR_TEST_ASSERT(jump->getDesc().format == rhi::Format::R8Uint);
        PPR_TEST_ASSERT(jump->getDesc().size.width == 1024u and jump->getDesc().size.height == 1024u);
        PPR_TEST_ASSERT(jump->getDesc().mipCount == 1u and jump->getDesc().sampleCount == 1u);

        rhi::FormatSupport support{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().getFormatSupport(rhi::Format::R8Uint, &support)));
        constexpr u32 required = static_cast<u32>(rhi::FormatSupport::Texture) |
                                 static_cast<u32>(rhi::FormatSupport::CopySource) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderLoad) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderUavLoad) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderUavStore);
        PPR_TEST_ASSERT((static_cast<u32>(support) & required) == required);

        std::println(
            "WORLD_LIGHTING_TRACE dispatch visibility={}x{}x1x256 jump={}x{}x1x256 mask_stride=8 mask_bytes={} "
            "jump_format=r8uint jump_texels={}",
            kVisibilityDispatchGroups, kVisibilityDispatchGroups, kJumpDispatchGroups, kJumpDispatchGroups,
            WorldLightingPass::kVisibilityBufferBytes, kEdge * kEdge);

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // numeric failure containment
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (nonfinite_trace_reports_numeric_failure) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto palette_values = makePalette();
        const auto palette = uploadPalette(fixture, palette_values);
        const auto source = uploadSource(fixture, 0u);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        CategoricalMap map;
        const auto attempt = encodeAcceleration(fixture, pass, palette, map);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        TraceProbe probe{fixture};
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();

        const std::array<float4, 4u> segments{
            makeSegment(float2{nan, 0.5f}, float2{10.5f, 0.5f}),
            makeSegment(float2{0.5f, 0.5f}, float2{infinity, 0.5f}),
            makeSegment(float2{-3.0e38f, 0.5f}, float2{3.0e38f, 0.5f}),
            makeSegment(float2{0.5f, 0.5f}, float2{10.5f, 0.5f}),
        };

        const auto result = probe.run(
            pass.resolvedCellsForTest(), palette.get(), attempt.m_snapshot.m_jump, source.get(),
            std::span<const float4>{segments.data(), segments.size()});

        for (u32 index = 0u; index < 3u; ++index) {
            const u32 base = index * 5u;
            PPR_TEST_ASSERT(result.m_words[base] == kNoNumericIndex);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 1u]) == 0.0f);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 2u]) == 0.0f);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 3u]) == 0.0f);
            PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[base + 4u]) == 0.0f);
        }

        // The valid segment stays a clear interval and contributes no failure.
        const u32 valid_base = 3u * 5u;
        PPR_TEST_ASSERT(result.m_words[valid_base] == kNoNumericIndex);
        PPR_TEST_ASSERT(std::bit_cast<float>(result.m_words[valid_base + 4u]) == 1.0f);

        // The smallest failing invocation index is the reported status index;
        // reserved words stay zero.
        PPR_TEST_ASSERT(result.m_status[0u] == 0u);
        PPR_TEST_ASSERT(result.m_status[1u] == kNoNumericIndex);
        for (u32 word = 2u; word < 8u; ++word) {
            PPR_TEST_ASSERT(result.m_status[word] == 0u);
        }

        // A later failure reports its own invocation index, not a clear result.
        const std::array<float4, 3u> later{
            makeSegment(float2{0.5f, 0.5f}, float2{10.5f, 0.5f}),
            makeSegment(float2{0.5f, 1.5f}, float2{10.5f, 1.5f}),
            makeSegment(float2{0.5f, 0.5f}, float2{infinity, 0.5f}),
        };
        const auto later_result = probe.run(
            pass.resolvedCellsForTest(), palette.get(), attempt.m_snapshot.m_jump, source.get(),
            std::span<const float4>{later.data(), later.size()});
        PPR_TEST_ASSERT(later_result.m_status[0u] == 2u);
        PPR_TEST_ASSERT(std::bit_cast<float>(later_result.m_words[2u * 5u + 4u]) == 0.0f);

        std::println("WORLD_LIGHTING_TRACE numeric_failure index_min=0 later_index=2 containment=(0,0)");
        PPR_TEST_ASSERT(not pass.shutdown());
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // world-lighting acceleration and segment tracer suite
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_traceTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_trace") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingTrace::accelerated_matches_native_cell_oracle,
                detail::WorldLightingTrace::visibility_masks_match_native_occupancy,
                detail::WorldLightingTrace::jump_is_largest_aligned_empty_block,
                detail::WorldLightingTrace::acceleration_rebuild_drops_previous_occupancy,
                detail::WorldLightingTrace::reflected_visibility_records_match_cpu_shapes,
                detail::WorldLightingTrace::nonfinite_trace_reports_numeric_failure,
            });
        };
        return group;
    }
}
