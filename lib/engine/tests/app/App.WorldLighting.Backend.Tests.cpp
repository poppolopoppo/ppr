module;

#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <slang.h>

module engine.tests.app;

import std;
import engine.core;
import engine.shader;
import engine.rhi;

namespace pP::tests::detail::WorldLightingBackend {
    // ------------------------------------------------------------------
    // backend fixture and record mirrors
    // ------------------------------------------------------------------

    // Qualification sizes follow the accepted records: a 1024-square R8_UINT
    // jump map, 32-byte metadata rows and the four-sample scene attachment.
    inline constexpr u32 kJumpExtent = 1024u;
    inline constexpr u32 kSampleWidth = 4u;
    inline constexpr u32 kSampleHeight = 2u;
    inline constexpr u32 kHistoryWidth = 4u;
    inline constexpr u32 kHistoryHeight = 2u;
    inline constexpr u32 kStatusWords = 8u;
    inline constexpr u32 kStatusThreads = 8u;
    inline constexpr u32 kNoStatusIndex = 0xffff'ffffu;

    struct IntervalRecord {
        u16 m_radiance_r{}, m_radiance_g{}, m_radiance_b{}, m_transmittance{};
    };

    struct FineRecord {
        u32 m_dc{};
        float m_scale{};
        u32 m_ac[6]{};
    };

    struct MetadataRecord {
        u32 m_id_low{}, m_id_high{}, m_source_row{}, m_material_temperature{};
        float m_gas_x{}, m_gas_y{}, m_radius{};
        u32 m_physical_fill_bits{};
    };

    static_assert(sizeof(IntervalRecord) == 8u and alignof(IntervalRecord) == 2u);
    static_assert(sizeof(FineRecord) == 32u and alignof(FineRecord) == 4u);
    static_assert(sizeof(MetadataRecord) == 32u and alignof(MetadataRecord) == 4u);
    static_assert(PPR_OFFSETOF(MetadataRecord, m_id_high) == 4u);
    static_assert(PPR_OFFSETOF(MetadataRecord, m_source_row) == 8u);
    static_assert(PPR_OFFSETOF(MetadataRecord, m_material_temperature) == 12u);
    static_assert(PPR_OFFSETOF(MetadataRecord, m_gas_x) == 16u);
    static_assert(PPR_OFFSETOF(MetadataRecord, m_gas_y) == 20u);
    static_assert(PPR_OFFSETOF(MetadataRecord, m_radius) == 24u);
    static_assert(PPR_OFFSETOF(MetadataRecord, m_physical_fill_bits) == 28u);

    struct ProbeParameters {
        u32 m_extent[2]{};
        u32 m_status_invalid_index{kNoStatusIndex};
        u32 m_status_range_index{kNoStatusIndex};
        u32 m_winner_cells{};
        u32 m_winner_candidates{};
        float m_cells_per_pixel{1.0f};
        u32 m_reserved{};
    };

    static_assert(sizeof(ProbeParameters) == 32u and PPR_OFFSETOF(ProbeParameters, m_cells_per_pixel) == 24u);

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue;

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
                    "WORLD_LIGHTING_BACKEND api={} adapter={} backend={} timestamp_hz={}",
                    info.apiName != nullptr ? info.apiName : "unknown",
                    info.adapterName != nullptr ? info.adapterName : "unknown", static_cast<int>(info.deviceType),
                    info.timestampFrequency));
        }

        void shutdown() {
            m_queue.setNull();
            PPR_TEST_ASSERT(not m_rhi->shutdown());
            PPR_TEST_ASSERT(not m_shaders->shutdown());
        }

        [[nodiscard]] rhi::IDevice &device() {
            return m_rhi->getDevice();
        }
    };

    // ------------------------------------------------------------------
    // program, binding and readback plumbing
    // ------------------------------------------------------------------

    [[nodiscard]] rhi::ComPtr<rhi::IShaderProgram> makeProgram(
        rhi::IDevice &device, shader::SharedModule &module, const std::span<const char *const> names) {
        std::array<shader::ComPtr<shader::IEntryPoint>, 2> entries{};
        std::array<shader::IComponentType *, 2> components{};
        PPR_TEST_ASSERT(names.size() <= entries.size());

        for (u32 index = 0u; index < names.size(); ++index) {
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(names[index], entries[index].writeRef())));
            components[index] = entries[index].get();
        }

        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = module.get();
        description.slangEntryPoints = components.data();
        description.slangEntryPointCount = static_cast<u32>(names.size());

        rhi::ComPtr<rhi::IShaderProgram> program;
        shader::Diagnose diagnostics;
        PPR_TEST_ASSERT(
            not make_error_code(device.createShaderProgram(description, program.writeRef(), diagnostics.writeRef())));
        return program;
    }

    struct ComputePipeline {
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline;
        rhi::ComPtr<rhi::IShaderObject> m_root;
    };

    [[nodiscard]] ComputePipeline makeComputePipeline(rhi::IDevice &device, rhi::IShaderProgram *const program) {
        ComputePipeline result{};
        rhi::ComputePipelineDesc description{};
        description.program = program;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(description, result.m_pipeline.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, result.m_root.writeRef())));
        return result;
    }

    void setProbe(rhi::IShaderObject &root, const ProbeParameters &parameters) {
        rhi::ShaderCursor cursor{&root};
        rhi::ShaderCursor constants{};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
    }

    void bindBuffer(rhi::IShaderObject &root, const char *const name, rhi::IBuffer *const buffer) {
        PPR_TEST_ASSERT(not make_error_code(rhi::ShaderCursor{&root}[name].setBinding(rhi::Binding(buffer))));
    }

    void bindTexture(rhi::IShaderObject &root, const char *const name, rhi::ITextureView *const view) {
        PPR_TEST_ASSERT(not make_error_code(rhi::ShaderCursor{&root}[name].setBinding(rhi::Binding(view))));
    }

    void submit(Fixture &fixture, rhi::ICommandEncoder &encoder) {
        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));

        bool submitted = true;
        PPR_DEFER {
            if (submitted) {
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
            }
        };

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        submitted = false;
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeStorageBuffer(
        rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial = nullptr) {
        rhi::BufferDesc description{};
        description.size = bytes;
        description.elementSize = stride;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::ShaderResource) |
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
            static_cast<u32>(rhi::BufferUsage::CopySource) |
            static_cast<u32>(rhi::BufferUsage::CopyDestination));
        description.defaultState = rhi::ResourceState::UnorderedAccess;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, initial, buffer.writeRef())));
        return buffer;
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeReadbackBuffer(rhi::IDevice &device, const u64 bytes) {
        rhi::BufferDesc description{};
        description.size = bytes;
        description.elementSize = 0u;
        description.memoryType = rhi::MemoryType::ReadBack;
        description.usage = rhi::BufferUsage::CopyDestination;
        description.defaultState = rhi::ResourceState::CopyDestination;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, buffer.writeRef())));
        return buffer;
    }

    [[nodiscard]] rhi::ComPtr<rhi::ITexture> makeTexture(rhi::IDevice &device, const rhi::TextureDesc &description) {
        rhi::ComPtr<rhi::ITexture> texture;
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(description, nullptr, texture.writeRef())));
        return texture;
    }

    [[nodiscard]] rhi::ComPtr<rhi::ITextureView> defaultView(const rhi::ComPtr<rhi::ITexture> &texture) {
        rhi::ComPtr<rhi::ITextureView> view;
        PPR_TEST_ASSERT(not make_error_code(texture->getDefaultView(view.writeRef())));
        return view;
    }

    struct TextureReadback {
        Array<std::byte> m_bytes{};
        u64 m_row_pitch{};
    };

    [[nodiscard]] TextureReadback readTexture(rhi::IDevice &device, rhi::ITexture &texture) {
        shader::ComPtr<ISlangBlob> blob{};
        rhi::SubresourceLayout layout{};
        PPR_TEST_ASSERT(not make_error_code(device.readTexture(&texture, 0u, 0u, blob.writeRef(), &layout)));
        PPR_TEST_ASSERT(blob.get() != nullptr and blob->getBufferPointer() != nullptr);

        TextureReadback readback{};
        readback.m_row_pitch = layout.rowPitch;
        const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());
        readback.m_bytes.assign(source, source + blob->getBufferSize());
        return readback;
    }

    [[nodiscard]] float halfToFloat(const u16 bits) noexcept {
        const u32 sign = (bits & 0x8000u) != 0u ? 0x8000'0000u : 0u;
        const u32 exponent = (bits >> 10u) & 0x1fu;
        const u32 mantissa = bits & 0x3ffu;

        if (exponent == 0u) {
            if (mantissa == 0u) {
                return std::bit_cast<float>(sign);
            }

            const float value = std::ldexp(static_cast<float>(mantissa), -24);
            return sign != 0u ? -value : value;
        }

        if (exponent == 31u) {
            if (mantissa != 0u) {
                return std::numeric_limits<float>::quiet_NaN();
            }

            return sign != 0u ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
        }

        return std::bit_cast<float>(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
    }

    // Reads one RGBA16F channel from an RGBA16F readback image.
    [[nodiscard]] float readHalf(const TextureReadback &pixels, const u32 x, const u32 y, const u32 channel) noexcept {
        const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels.m_row_pitch) +
                                   static_cast<std::size_t>(x) * 8u +
                                   static_cast<std::size_t>(channel) * 2u;
        u16 bits{};
        std::memcpy(&bits, pixels.m_bytes.data() + offset, sizeof(bits));
        return halfToFloat(bits);
    }

    [[nodiscard]] rhi::ComPtr<rhi::ICommandEncoder> makeEncoder(Fixture &fixture) {
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        return encoder;
    }

    // ------------------------------------------------------------------
    // R8_UINT jump storage
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (r8_uint_jump_storage_round_trips_all_seven_values) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        auto &device = fixture.device();

        rhi::FormatSupport support{};
        PPR_TEST_ASSERT(not make_error_code(device.getFormatSupport(rhi::Format::R8Uint, &support)));
        constexpr u32 required = static_cast<u32>(rhi::FormatSupport::Texture) |
                                 static_cast<u32>(rhi::FormatSupport::CopySource) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderLoad) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderUavLoad) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderUavStore);
        _.log(
            std::format(
                "WORLD_LIGHTING_BACKEND r8_uint_support=0x{:x} required=0x{:x}", static_cast<u32>(support), required));
        PPR_TEST_ASSERT((static_cast<u32>(support) & required) == required);

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_backend.slang",
            "world_lighting_backend", module.writeRef()));

        constexpr const char *const store_entries[] = {"storeJumps"};
        constexpr const char *const load_entries[] = {"loadJumps"};
        const auto store_program = makeProgram(device, module, store_entries);
        const auto load_program = makeProgram(device, module, load_entries);
        auto store = makeComputePipeline(device, store_program);
        auto load = makeComputePipeline(device, load_program);

        rhi::TextureDesc jump_description{};
        jump_description.type = rhi::TextureType::Texture2D;
        jump_description.size = {kJumpExtent, kJumpExtent, 1u};
        jump_description.arrayLength = 1u;
        jump_description.mipCount = 1u;
        jump_description.format = rhi::Format::R8Uint;
        jump_description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::ShaderResource) | static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
        jump_description.defaultState = rhi::ResourceState::UnorderedAccess;
        jump_description.label = "world-lighting jump qualification";
        const auto jumps = makeTexture(device, jump_description);
        const auto jump_view = defaultView(jumps);
        PPR_TEST_ASSERT(jumps->getDesc().format == rhi::Format::R8Uint);
        PPR_TEST_ASSERT(jumps->getDesc().mipCount == 1u and jumps->getDesc().sampleCount == 1u);

        const auto jump_out = makeStorageBuffer(device, u64{kJumpExtent} * kJumpExtent * sizeof(u32), sizeof(u32));

        ProbeParameters parameters{};
        parameters.m_extent[0] = kJumpExtent;
        parameters.m_extent[1] = kJumpExtent;
        setProbe(*store.m_root, parameters);
        bindTexture(*store.m_root, "g_jump_write", jump_view);
        setProbe(*load.m_root, parameters);
        bindTexture(*load.m_root, "g_jump_read", jump_view);
        bindBuffer(*load.m_root, "g_jump_out", jump_out);

        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        auto encoder = makeEncoder(fixture);
        encoder->setTextureState(jumps, range, rhi::ResourceState::UnorderedAccess);
        {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(store.m_pipeline, store.m_root);
            pass->dispatchCompute(kJumpExtent / 16u, kJumpExtent / 16u, 1u);
            pass->end();
        }

        // Ordered UAV write -> SRV read transition on the same R8 texel.
        encoder->setTextureState(jumps, range, rhi::ResourceState::ShaderResource);
        {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(load.m_pipeline, load.m_root);
            pass->dispatchCompute(kJumpExtent / 16u, kJumpExtent / 16u, 1u);
            pass->end();
        }

        submit(fixture, *encoder);

        Array<u32> texels(static_cast<std::size_t>(kJumpExtent) * kJumpExtent);
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(jump_out, 0u, texels.size() * sizeof(u32), texels.data())));

        constexpr std::array<u32, 7> jump_table{0u, 1u, 2u, 4u, 8u, 16u, 32u};
        for (u32 y = 0u; y < kJumpExtent; ++y) {
            for (u32 x = 0u; x < kJumpExtent; ++x) {
                PPR_TEST_ASSERT(texels[y * kJumpExtent + x] == jump_table[(x + y * 3u) % jump_table.size()]);
            }
        }

        std::array<u32, 7> jump_values{};
        for (u32 index = 0u; index < jump_values.size(); ++index) {
            jump_values[index] = texels[index];
        }

        PPR_TEST_ASSERT(jump_values == std::array{0u, 1u, 2u, 4u, 8u, 16u, 32u});
        _.log(
            std::format(
                "WORLD_LIGHTING_BACKEND r8_texels={} jump_values={},{},{},{},{},{},{}", texels.size(), jump_values[0],
                jump_values[1], jump_values[2], jump_values[3], jump_values[4], jump_values[5], jump_values[6]));
    };

    // ------------------------------------------------------------------
    // 32-bit atomic-min winner stability
    // ------------------------------------------------------------------

    struct WinnerCandidate {
        u32 m_cell{}, m_key{}, m_row{};
    };

    static_assert(sizeof(WinnerCandidate) == 12u);

    struct WinnerResult {
        Array<u32> m_keys{};
        Array<u32> m_rows{};
    };

    // Two row-major uint32 scratch grids (best_key/best_row) with exact binary32
    // distance ties resolved by the stable-ID row rank. Every atomic is 32-bit.
    [[nodiscard]] WinnerResult qualifyWinners(
        Fixture &fixture, shader::SharedModule &module, const std::span<const WinnerCandidate> candidates) {
        auto &device = fixture.device();
        constexpr const char *const clear_entries[] = {"clearWinners"};
        constexpr const char *const key_entries[] = {"reduceWinnerKeys"};
        constexpr const char *const row_entries[] = {"reduceWinnerRows"};
        const auto clear_program = makeProgram(device, module, clear_entries);
        const auto key_program = makeProgram(device, module, key_entries);
        const auto row_program = makeProgram(device, module, row_entries);
        auto clear = makeComputePipeline(device, clear_program);
        auto key = makeComputePipeline(device, key_program);
        auto row = makeComputePipeline(device, row_program);

        constexpr u32 cells = 4u;
        const auto candidate_buffer = makeStorageBuffer(
            device, candidates.size() * sizeof(WinnerCandidate), sizeof(WinnerCandidate), candidates.data());
        const auto best_key = makeStorageBuffer(device, cells * sizeof(u32), sizeof(u32));
        const auto best_row = makeStorageBuffer(device, cells * sizeof(u32), sizeof(u32));
        PPR_TEST_ASSERT(best_key->getDesc().elementSize == 4u and best_row->getDesc().elementSize == 4u);

        ProbeParameters parameters{};
        parameters.m_winner_cells = cells;
        parameters.m_winner_candidates = static_cast<u32>(candidates.size());
        setProbe(*clear.m_root, parameters);
        bindBuffer(*clear.m_root, "g_best_key", best_key);
        bindBuffer(*clear.m_root, "g_best_row", best_row);
        setProbe(*key.m_root, parameters);
        bindBuffer(*key.m_root, "g_candidates", candidate_buffer);
        bindBuffer(*key.m_root, "g_best_key", best_key);
        setProbe(*row.m_root, parameters);
        bindBuffer(*row.m_root, "g_candidates", candidate_buffer);
        bindBuffer(*row.m_root, "g_best_key", best_key);
        bindBuffer(*row.m_root, "g_best_row", best_row);

        auto encoder = makeEncoder(fixture);
        const auto dispatch = [&](ComputePipeline &pipeline) {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(pipeline.m_pipeline, pipeline.m_root);
            pass->dispatchCompute(1u, 1u, 1u);
            pass->end();
            encoder->globalBarrier();
        };

        dispatch(clear);
        dispatch(key);
        dispatch(row);
        submit(fixture, *encoder);

        WinnerResult result{};
        result.m_keys.resize(cells);
        result.m_rows.resize(cells);
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(best_key, 0u, cells * sizeof(u32), result.m_keys.data())));
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(best_row, 0u, cells * sizeof(u32), result.m_rows.data())));
        return result;
    }

    PPR_UNIT_TEST (atomic_min_winner_is_stable_under_reordered_dispatch_input) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_backend.slang",
            "world_lighting_backend", module.writeRef()));

        // Cell 0 and cell 3 hold two exact 32-bit key ties; the lower stable-ID
        // row wins. Cell 2 keeps the class bit ahead of a nearer plain key.
        constexpr std::array<WinnerCandidate, 8> candidates{{
            {0u, 0x0000'0100u, 5u},
            {0u, 0x0000'0100u, 2u},
            {1u, 0x0000'0010u, 9u},
            {1u, 0x0000'0020u, 3u},
            {2u, 0x8000'0005u, 1u},
            {2u, 0x0000'0030u, 7u},
            {3u, 0x8000'0005u, 4u},
            {3u, 0x8000'0005u, 3u},
        }};

        constexpr std::array<u32, 4> expected_keys{0x0000'0100u, 0x0000'0010u, 0x0000'0030u, 0x8000'0005u};
        constexpr std::array<u32, 4> expected_rows{2u, 9u, 7u, 3u};

        const auto verify = [&](const WinnerResult &result, const char *const order) {
            for (u32 cell = 0u; cell < expected_keys.size(); ++cell) {
                PPR_TEST_ASSERT(result.m_keys[cell] == expected_keys[cell]);
                PPR_TEST_ASSERT(result.m_rows[cell] == expected_rows[cell]);
            }

            _.log(
                std::format(
                    "WORLD_LIGHTING_BACKEND winner_order={} keys={:x},{:x},{:x},{:x} rows={},{},{},{}", order,
                    result.m_keys[0], result.m_keys[1], result.m_keys[2], result.m_keys[3], result.m_rows[0],
                    result.m_rows[1], result.m_rows[2], result.m_rows[3]));
        };

        const auto forward = qualifyWinners(fixture, module, candidates);
        verify(forward, "forward");

        std::array<WinnerCandidate, candidates.size()> reversed{};
        std::ranges::reverse_copy(candidates, reversed.begin());
        const auto backward = qualifyWinners(fixture, module, reversed);
        verify(backward, "reversed");

        constexpr std::array<u32, 8> permutation{3u, 7u, 1u, 5u, 0u, 6u, 2u, 4u};
        std::array<WinnerCandidate, candidates.size()> shuffled{};
        for (u32 index = 0u; index < permutation.size(); ++index) {
            shuffled[index] = candidates[permutation[index]];
        }

        const auto permuted = qualifyWinners(fixture, module, shuffled);
        verify(permuted, "permuted");

        PPR_TEST_ASSERT(forward.m_keys == backward.m_keys and forward.m_keys == permuted.m_keys);
        PPR_TEST_ASSERT(forward.m_rows == backward.m_rows and forward.m_rows == permuted.m_rows);
    };

    // ------------------------------------------------------------------
    // reflected records
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (reflected_structured_strides_and_metadata_offsets_match_records) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        auto &device = fixture.device();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_backend.slang",
            "world_lighting_backend", module.writeRef()));

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

        for (const auto &[name, stride]: std::array<std::pair<const char *, u32>, 6u>{{
                 {"g_keys", 4u},
                 {"g_best_key", 4u},
                 {"g_status", 4u},
                 {"g_intervals", 8u},
                 {"g_fine_ch", 32u},
                 {"g_metadata", 32u},
             }}) {
            auto *const type = element(name);
            PPR_TEST_ASSERT(type != nullptr);
            _.log(
                std::format(
                    "WORLD_LIGHTING_BACKEND reflected name={} stride={} size={} expected_stride={}", name,
                    type->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM), type->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM),
                    stride));
            PPR_TEST_ASSERT(type->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == stride);
        }

        auto *const interval = element("g_intervals");
        auto *const fine = element("g_fine_ch");
        auto *const metadata = element("g_metadata");
        PPR_TEST_ASSERT(interval != nullptr and interval->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u);
        PPR_TEST_ASSERT(fine != nullptr and fine->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u);
        PPR_TEST_ASSERT(metadata != nullptr and metadata->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u);
        PPR_TEST_ASSERT(fine->getFieldByIndex(0u)->getOffset() == 0u);
        PPR_TEST_ASSERT(fine->getFieldByIndex(1u)->getOffset() == 4u);
        for (u32 field = 0u; field < 6u; ++field) {
            PPR_TEST_ASSERT(fine->getFieldByIndex(2u + field)->getOffset() == 8u + field * 4u);
        }

        PPR_TEST_ASSERT(metadata->getFieldCount() == 8u);
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 8u>{{
                 {"m_id_low", 0u},
                 {"m_id_high", 4u},
                 {"m_source_row", 8u},
                 {"m_material_temperature", 12u},
                 {"m_gas_x", 16u},
                 {"m_gas_y", 20u},
                 {"m_radius", 24u},
                 {"m_physical_fill_bits", 28u},
             }}) {
            bool matched = false;
            for (u32 index = 0u; index < metadata->getFieldCount(); ++index) {
                auto *const field = metadata->getFieldByIndex(index);
                if (name == std::string_view{field->getName()}) {
                    PPR_TEST_ASSERT(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset);
                    matched = true;
                }
            }

            PPR_TEST_ASSERT(matched);
        }

        // Reflection alone is not execution: create one real compute pipeline
        // from the same module while its layout is qualified.
        constexpr const char *const entries[] = {"reduceWinnerKeys"};
        const auto program = makeProgram(device, module, entries);
        auto pipeline = makeComputePipeline(device, program);
        PPR_TEST_ASSERT(pipeline.m_pipeline.get() != nullptr and pipeline.m_root.get() != nullptr);

        _.log(std::format("WORLD_LIGHTING_BACKEND strides=4/8/32 metadata_offsets=0/4/8/12/16/20/24/28"));
    };

    // ------------------------------------------------------------------
    // sample-rate execution and pattern
    // ------------------------------------------------------------------

    struct SamplePrograms {
        rhi::ComPtr<rhi::IShaderProgram> m_graphics{};
        rhi::ComPtr<rhi::IRenderPipeline> m_raster{};
        rhi::ComPtr<rhi::IShaderObject> m_graphics_root{};
        rhi::ComPtr<rhi::IShaderProgram> m_dump_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_dump_pipeline{};
        rhi::ComPtr<rhi::IShaderObject> m_dump_root{};
        rhi::ComPtr<rhi::ITexture> m_samples{};
        rhi::ComPtr<rhi::ITexture> m_dump_texture{};
        rhi::ComPtr<rhi::ITextureView> m_samples_view{};
        rhi::ComPtr<rhi::ITextureView> m_dump_view{};

        SamplePrograms(Fixture &fixture, const char *const fragment_entry, const float cells_per_pixel) {
            auto &device = fixture.device();
            shader::SharedModule module{};
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_backend.slang",
                "world_lighting_backend", module.writeRef()));

            const char *const graphics_entries[] = {"vertexSampleProbe", fragment_entry};
            constexpr const char *const dump_entries[] = {"dumpSamples"};
            m_graphics = makeProgram(device, module, graphics_entries);
            m_dump_program = makeProgram(device, module, dump_entries);

            rhi::RenderPipelineDesc raster_description{};
            raster_description.program = m_graphics;
            rhi::ColorTargetDesc color{};
            color.format = rhi::Format::RGBA16Float;
            raster_description.targets = &color;
            raster_description.targetCount = 1u;
            raster_description.multisample.sampleCount = 4u;
            raster_description.rasterizer.cullMode = rhi::CullMode::None;
            raster_description.depthStencil.depthTestEnable = false;
            raster_description.depthStencil.depthWriteEnable = false;
            PPR_TEST_ASSERT(not make_error_code(device.createRenderPipeline(raster_description, m_raster.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(m_graphics, m_graphics_root.writeRef())));

            const auto dump = makeComputePipeline(device, m_dump_program);
            m_dump_pipeline = dump.m_pipeline;
            m_dump_root = dump.m_root;

            rhi::TextureDesc sample_description{};
            sample_description.type = rhi::TextureType::Texture2DMS;
            sample_description.size = {kSampleWidth, kSampleHeight, 1u};
            sample_description.arrayLength = 1u;
            sample_description.mipCount = 1u;
            sample_description.format = rhi::Format::RGBA16Float;
            sample_description.sampleCount = 4u;
            sample_description.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::RenderTarget) |
                static_cast<u32>(rhi::TextureUsage::ShaderResource));
            sample_description.defaultState = rhi::ResourceState::RenderTarget;
            sample_description.label = "world-lighting sample-rate target";
            m_samples = makeTexture(device, sample_description);

            rhi::TextureDesc dump_description = sample_description;
            dump_description.type = rhi::TextureType::Texture2D;
            dump_description.size = {kSampleWidth * 4u, kSampleHeight, 1u};
            dump_description.sampleCount = 1u;
            dump_description.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                static_cast<u32>(rhi::TextureUsage::UnorderedAccess) |
                static_cast<u32>(rhi::TextureUsage::CopySource));
            dump_description.defaultState = rhi::ResourceState::UnorderedAccess;
            dump_description.label = "world-lighting sample-rate dump";
            m_dump_texture = makeTexture(device, dump_description);
            m_samples_view = defaultView(m_samples);
            m_dump_view = defaultView(m_dump_texture);

            ProbeParameters parameters{};
            parameters.m_extent[0] = kSampleWidth;
            parameters.m_extent[1] = kSampleHeight;
            parameters.m_cells_per_pixel = cells_per_pixel;
            setProbe(*m_graphics_root, parameters);
            setProbe(*m_dump_root, parameters);
            bindTexture(*m_dump_root, "g_msaa_color", m_samples_view);
            bindTexture(*m_dump_root, "g_sample_dump", m_dump_view);
        }

        [[nodiscard]] TextureReadback run(Fixture &fixture) {
            const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
            auto encoder = makeEncoder(fixture);
            {
                rhi::RenderPassColorAttachment attachment{
                    .view = m_samples_view.get(), .clearValue = {0.0, 0.0, 0.0, 0.0}};
                rhi::RenderPassDesc description{};
                description.colorAttachments = &attachment;
                description.colorAttachmentCount = 1u;

                auto *const pass = encoder->beginRenderPass(description);
                PPR_TEST_ASSERT(pass != nullptr);
                PPR_DEFER {
                    pass->end();
                };

                pass->setRenderState(
                    {.viewports = {rhi::Viewport::fromSize(float(kSampleWidth), float(kSampleHeight))},
                        .viewportCount = 1u,
                        .scissorRects = {rhi::ScissorRect::fromSize(kSampleWidth, kSampleHeight)},
                        .scissorRectCount = 1u,
                        .vertexBuffers = {},
                        .indexBuffer = {}});
                pass->bindPipeline(m_raster, m_graphics_root);
                pass->draw({.vertexCount = 3u, .instanceCount = 1u});
            }

            encoder->setTextureState(m_samples, range, rhi::ResourceState::ShaderResource);
            {
                auto *const pass = encoder->beginComputePass();
                PPR_TEST_ASSERT(pass != nullptr);
                pass->bindPipeline(m_dump_pipeline, m_dump_root);
                pass->dispatchCompute(1u, 1u, 1u);
                pass->end();
            }

            submit(fixture, *encoder);
            return readTexture(fixture.device(), *m_dump_texture);
        }
    };

    // Standard 4x pattern in raster-pixel top-left coordinates.
    constexpr std::array<std::array<double, 2>, 4> kSamplePositions{{
        {3.0 / 8.0, 1.0 / 8.0},
        {7.0 / 8.0, 3.0 / 8.0},
        {1.0 / 8.0, 5.0 / 8.0},
        {5.0 / 8.0, 7.0 / 8.0},
    }};

    void qualifySampleFeature(UnitTest::IRun &_, rhi::IDevice &device) {
        _.log(
            std::format(
                "WORLD_LIGHTING_BACKEND sample_rate_shading={} standard_sample_locations={}",
                device.hasFeature("sample-rate-shading"), device.hasFeature("standard-sample-locations-4")));
        PPR_TEST_ASSERT(device.hasFeature("sample-rate-shading"));
        PPR_TEST_ASSERT(device.hasFeature("standard-sample-locations-4"));
    }

    PPR_UNIT_TEST (sample_rate_positions_match_standard_top_left_pattern) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        qualifySampleFeature(_, fixture.device());

        SamplePrograms programs{fixture, "fragmentSamplePositions", 1.0f};
        const auto pixels = programs.run(fixture);

        for (u32 y = 0u; y < kSampleHeight; ++y) {
            for (u32 x = 0u; x < kSampleWidth; ++x) {
                for (u32 sample = 0u; sample < 4u; ++sample) {
                    const double measured_x = static_cast<double>(readHalf(pixels, x * 4u + sample, y, 0u));
                    const double measured_y = static_cast<double>(readHalf(pixels, x * 4u + sample, y, 1u));
                    const double expected_x = static_cast<double>(x) + kSamplePositions[sample][0];
                    const double expected_y = static_cast<double>(y) + kSamplePositions[sample][1];

                    PPR_TEST_ASSERT(std::abs(measured_x - expected_x) <= 1e-4);
                    PPR_TEST_ASSERT(std::abs(measured_y - expected_y) <= 1e-4);
                    PPR_TEST_ASSERT(readHalf(pixels, x * 4u + sample, y, 2u) == static_cast<float>(sample));

                    _.log(
                        std::format(
                            "WORLD_LIGHTING_BACKEND sample_pixel=({}, {}) sample={} position=({:.6f}, {:.6f})", x, y,
                            sample, measured_x, measured_y));
                }
            }
        }
    };

    PPR_UNIT_TEST (sample_rate_cell_boundary_selects_distinct_samples) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        qualifySampleFeature(_, fixture.device());

        // Two half-pixel cells per pixel put a material boundary inside every
        // raster pixel: samples 0/2 select cell 2x, samples 1/3 select 2x+1.
        SamplePrograms programs{fixture, "fragmentSampleCells", 2.0f};
        const auto pixels = programs.run(fixture);

        for (u32 y = 0u; y < kSampleHeight; ++y) {
            for (u32 x = 0u; x < kSampleWidth; ++x) {
                std::array<float, 4> red{};
                for (u32 sample = 0u; sample < 4u; ++sample) {
                    const u32 cell = static_cast<u32>(
                        std::floor((static_cast<double>(x) + kSamplePositions[sample][0]) * 2.0));
                    const float expected_red = (cell % 2u) == 0u ? 1.0f : 0.0f;
                    const float expected_green = (cell % 2u) == 0u ? 0.0f : 1.0f;

                    red[sample] = readHalf(pixels, x * 4u + sample, y, 0u);
                    PPR_TEST_ASSERT(red[sample] == expected_red);
                    PPR_TEST_ASSERT(readHalf(pixels, x * 4u + sample, y, 1u) == expected_green);
                    PPR_TEST_ASSERT(readHalf(pixels, x * 4u + sample, y, 2u) == 0.0f);
                    PPR_TEST_ASSERT(readHalf(pixels, x * 4u + sample, y, 3u) == 1.0f);
                }

                PPR_TEST_ASSERT(red[0] == red[2] and red[1] == red[3]);
                PPR_TEST_ASSERT(red[0] != red[1]);
                _.log(
                    std::format(
                        "WORLD_LIGHTING_BACKEND boundary_pixel=({}, {}) sample_colors={},{},{},{}", x, y, red[0],
                        red[1], red[2], red[3]));
            }
        }
    };

    // ------------------------------------------------------------------
    // history ordering and status-copy dependency
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (history_uav_srv_transitions_preserve_sequential_values) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        auto &device = fixture.device();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_backend.slang",
            "world_lighting_backend", module.writeRef()));

        constexpr const char *const write_entries[] = {"writeHistory"};
        constexpr const char *const advance_entries[] = {"advanceHistory"};
        const auto write_program = makeProgram(device, module, write_entries);
        const auto advance_program = makeProgram(device, module, advance_entries);
        auto write = makeComputePipeline(device, write_program);
        auto advance_ab = makeComputePipeline(device, advance_program);
        auto advance_ba = makeComputePipeline(device, advance_program);

        const auto make_history = [&](const char *const label) {
            rhi::TextureDesc description{};
            description.type = rhi::TextureType::Texture2D;
            description.size = {kHistoryWidth, kHistoryHeight, 1u};
            description.arrayLength = 1u;
            description.mipCount = 1u;
            description.format = rhi::Format::RGBA16Float;
            description.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                static_cast<u32>(rhi::TextureUsage::UnorderedAccess) |
                static_cast<u32>(rhi::TextureUsage::CopySource));
            description.defaultState = rhi::ResourceState::UnorderedAccess;
            description.label = label;
            return makeTexture(device, description);
        };
        const auto history_a = make_history("world-lighting history A");
        const auto history_b = make_history("world-lighting history B");
        const auto view_a = defaultView(history_a);
        const auto view_b = defaultView(history_b);

        ProbeParameters parameters{};
        parameters.m_extent[0] = kHistoryWidth;
        parameters.m_extent[1] = kHistoryHeight;
        setProbe(*write.m_root, parameters);
        bindTexture(*write.m_root, "g_history_next", view_a);
        setProbe(*advance_ab.m_root, parameters);
        bindTexture(*advance_ab.m_root, "g_history_prior", view_a);
        bindTexture(*advance_ab.m_root, "g_history_next", view_b);

        // Each recorded dispatch keeps its own root: no recorded binding is
        // rewritten before the single submission.
        setProbe(*advance_ba.m_root, parameters);
        bindTexture(*advance_ba.m_root, "g_history_prior", view_b);
        bindTexture(*advance_ba.m_root, "g_history_next", view_a);

        const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
        auto encoder = makeEncoder(fixture);
        const auto dispatch = [&](ComputePipeline &pipeline) {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(pipeline.m_pipeline, pipeline.m_root);
            pass->dispatchCompute(1u, 1u, 1u);
            pass->end();
        };

        encoder->setTextureState(history_a, range, rhi::ResourceState::UnorderedAccess);
        dispatch(write);

        // Frame 1: A is read as an SRV while B is written as a UAV.
        encoder->setTextureState(history_a, range, rhi::ResourceState::ShaderResource);
        encoder->setTextureState(history_b, range, rhi::ResourceState::UnorderedAccess);
        dispatch(advance_ab);

        // Frame 2: the roles swap, so B becomes the SRV and A the UAV again.
        encoder->setTextureState(history_b, range, rhi::ResourceState::ShaderResource);
        encoder->setTextureState(history_a, range, rhi::ResourceState::UnorderedAccess);
        dispatch(advance_ba);

        submit(fixture, *encoder);

        const auto read_pixel = [&](const rhi::ComPtr<rhi::ITexture> &texture, const u32 channel) {
            const auto pixels = readTexture(device, *texture);
            const float value = readHalf(pixels, 0u, 0u, channel);
            for (u32 y = 0u; y < kHistoryHeight; ++y) {
                for (u32 x = 0u; x < kHistoryWidth; ++x) {
                    PPR_TEST_ASSERT(readHalf(pixels, x, y, channel) == value);
                }
            }

            return value;
        };

        PPR_TEST_ASSERT(read_pixel(history_a, 0u) == 3.0f);
        PPR_TEST_ASSERT(read_pixel(history_b, 0u) == 2.0f);
        PPR_TEST_ASSERT(read_pixel(history_a, 1u) == 0.5f);
        PPR_TEST_ASSERT(read_pixel(history_a, 2u) == 0.25f);
        PPR_TEST_ASSERT(read_pixel(history_a, 3u) == 1.0f);
        _.log(std::format("WORLD_LIGHTING_BACKEND history_a_r=3 history_b_r=2 ordered=true"));
    };

    PPR_UNIT_TEST (status_copy_reports_atomic_minima_and_reserved_zeroes) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);
        auto &device = fixture.device();

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_backend.slang",
            "world_lighting_backend", module.writeRef()));

        constexpr const char *const entries[] = {"writeStatus"};
        const auto program = makeProgram(device, module, entries);
        auto first_status = makeComputePipeline(device, program);
        auto second_status = makeComputePipeline(device, program);

        constexpr std::array<u32, kStatusWords> initial{0xffff'ffffu, 0xffff'ffffu, 0u, 0u, 0u, 0u, 0u, 0u};
        const auto status_buffer = makeStorageBuffer(device, sizeof(initial), sizeof(u32), initial.data());
        const std::array<u32, kStatusThreads> zeros{};
        const auto result_buffer = makeStorageBuffer(device, sizeof(zeros), sizeof(u32), zeros.data());
        const auto readback = makeReadbackBuffer(device, sizeof(initial));

        const auto configure = [&](ComputePipeline &pipeline, const u32 invalid_index, const u32 range_index) {
            ProbeParameters parameters{};
            parameters.m_extent[0] = kStatusThreads;
            parameters.m_extent[1] = 1u;
            parameters.m_status_invalid_index = invalid_index;
            parameters.m_status_range_index = range_index;
            setProbe(*pipeline.m_root, parameters);
            bindBuffer(*pipeline.m_root, "g_status", status_buffer);
            bindBuffer(*pipeline.m_root, "g_status_result", result_buffer);
        };

        auto encoder = makeEncoder(fixture);
        const auto dispatch = [&](ComputePipeline &pipeline) {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(pipeline.m_pipeline, pipeline.m_root);
            pass->dispatchCompute(1u, 1u, 1u);
            pass->end();
            encoder->globalBarrier();
        };

        // Two failing invocations per category: the record keeps the smaller
        // 32-bit index, and the failing invocations publish a finite zero result.
        configure(first_status, 6u, 5u);
        configure(second_status, 2u, 3u);
        dispatch(first_status);
        dispatch(second_status);

        encoder->setBufferState(status_buffer, rhi::ResourceState::CopySource);
        encoder->copyBuffer(readback, 0u, status_buffer, 0u, sizeof(initial));
        encoder->setBufferState(status_buffer, rhi::ResourceState::UnorderedAccess);
        submit(fixture, *encoder);

        void *mapped{};
        PPR_TEST_ASSERT(not make_error_code(device.mapBuffer(readback, rhi::CpuAccessMode::Read, &mapped)));
        std::array<u32, kStatusWords> record{};
        std::memcpy(record.data(), mapped, sizeof(record));
        PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(readback)));

        PPR_TEST_ASSERT(record[0] == 2u and record[1] == 3u);
        for (u32 word = 2u; word < record.size(); ++word) {
            PPR_TEST_ASSERT(record[word] == 0u);
        }

        std::array<u32, kStatusThreads> results{};
        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(result_buffer, 0u, sizeof(results), results.data())));
        for (u32 index = 0u; index < results.size(); ++index) {
            const bool rejected = index == 2u or index == 3u;
            PPR_TEST_ASSERT(results[index] == (rejected ? 0u : 1u));
        }

        _.log(
            std::format(
                "WORLD_LIGHTING_BACKEND status_invalid_min={} status_range_min={} reserved_zero=true", record[0],
                record[1]));
    };
} // namespace pP::tests::detail::WorldLightingBackend

namespace pP::tests {
    // ------------------------------------------------------------------
    // world-lighting backend qualification
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_backendTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_backend") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingBackend::r8_uint_jump_storage_round_trips_all_seven_values,
                detail::WorldLightingBackend::atomic_min_winner_is_stable_under_reordered_dispatch_input,
                detail::WorldLightingBackend::reflected_structured_strides_and_metadata_offsets_match_records,
                detail::WorldLightingBackend::sample_rate_positions_match_standard_top_left_pattern,
                detail::WorldLightingBackend::sample_rate_cell_boundary_selects_distinct_samples,
                detail::WorldLightingBackend::history_uav_srv_transitions_preserve_sequential_values,
                detail::WorldLightingBackend::status_copy_reports_atomic_minima_and_reserved_zeroes,
            });
        };
        return group;
    }
}
