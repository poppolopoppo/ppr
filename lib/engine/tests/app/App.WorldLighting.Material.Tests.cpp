module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <slang.h>

module engine.tests.app;

import std;
import engine.core;
import engine.app;
import engine.image;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP::tests::detail::WorldLightingMaterial {
    // ------------------------------------------------------------------
    // fixture values
    // ------------------------------------------------------------------

    inline constexpr u32 kWidth = 64u;
    inline constexpr u32 kHeight = 128u;
    inline constexpr u32 kSampleCount = 1024u;
    inline constexpr double kPi = 3.14159265358979323846;
    inline constexpr double kInvSqrtPi = 0.56418958354775628694;
    inline constexpr double kInvSqrtTwoPi = 0.39894228040143267794;

    static_assert(lighting::kBrdfLutWidth == kWidth);
    static_assert(lighting::kBrdfLutHeight == kHeight);
    static_assert(lighting::kBrdfLutBytes == 32768u);
    static_assert(lighting::kBrdfLutSampleCount == kSampleCount);
    static_assert(WorldLightingPass::kWorldEdge == 1024u);

    [[nodiscard]] float halfToFloat(const u16 bits) noexcept {
        const u32 sign = (bits & 0x8000u) != 0u ? 0x8000'0000u : 0u;
        const u32 exponent = (bits >> 10u) & 0x1fu;
        const u32 mantissa = bits & 0x3ffu;
        if (exponent == 0u) {
            if (mantissa == 0u)
                return std::bit_cast<float>(sign);
            u32 shifted = mantissa;
            u32 shift = 0u;
            while ((shifted & 0x400u) == 0u) {
                shifted <<= 1u;
                ++shift;
            }
            return std::bit_cast<float>(sign | ((113u - shift) << 23u) | ((shifted & 0x3ffu) << 13u));
        }
        if (exponent == 31u)
            return std::bit_cast<float>(sign | 0x7f80'0000u | (mantissa << 13u));
        return std::bit_cast<float>(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
    }

    struct Double3 {
        double x{}, y{}, z{};
    };

    struct Double2 {
        double x{}, y{};
    };

    [[nodiscard]] Double3 normalized(const Double3 value, const Double3 fallback) noexcept {
        const double squared = value.x * value.x + value.y * value.y + value.z * value.z;
        if (!(squared > 1.0e-12))
            return fallback;
        const double inverse = 1.0 / std::sqrt(squared);
        return {value.x * inverse, value.y * inverse, value.z * inverse};
    }

    [[nodiscard]] double radicalInverse(u32 bits) noexcept {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x5555'5555u) << 1u) | ((bits & 0xAAAA'AAAAu) >> 1u);
        bits = ((bits & 0x3333'3333u) << 2u) | ((bits & 0xCCCC'CCCCu) >> 2u);
        bits = ((bits & 0x0F0F'0F0Fu) << 4u) | ((bits & 0xF0F0'F0F0u) >> 4u);
        bits = ((bits & 0x00FF'00FFu) << 8u) | ((bits & 0xFF00'FF00u) >> 8u);
        return static_cast<double>(bits) * 2.3283064365386963e-10;
    }

    [[nodiscard]] Double2 hammersley(const u32 index) noexcept {
        return {std::fmod(static_cast<double>(index) * 0.6180339887, 1.0), radicalInverse(index)};
    }

    [[nodiscard]] Double3 vndfSample(const Double2 xi, const double alpha, const Double3 view) noexcept {
        Double3 stretched = normalized({alpha * view.x, alpha * view.y, view.z}, {0.0, 0.0, 1.0});
        Double3 tangent{};
        if (stretched.z < 0.999) {
            const Double3 cross{stretched.y, -stretched.x, 0.0};
            tangent = normalized(cross, {1.0, 0.0, 0.0});
        } else {
            tangent = {1.0, 0.0, 0.0};
        }
        const Double3 bitangent{
            tangent.y * stretched.z - tangent.z * stretched.y, tangent.z * stretched.x - tangent.x * stretched.z,
            tangent.x * stretched.y - tangent.y * stretched.x};

        const double radius = std::sqrt((std::max) (xi.x, 0.0));
        const double phi = 2.0 * kPi * xi.y;
        const double t1 = radius * std::cos(phi);
        double t2 = radius * std::sin(phi);
        const double projected = 0.5 * (1.0 + stretched.z);
        t2 = (1.0 - projected) * std::sqrt((std::max) (1.0 - t1 * t1, 0.0)) + projected * t2;
        const double remaining = std::sqrt((std::max) (0.0, 1.0 - t1 * t1 - t2 * t2));
        const Double3 hemisphere{
            t1 * tangent.x + t2 * bitangent.x + remaining * stretched.x,
            t1 * tangent.y + t2 * bitangent.y + remaining * stretched.y,
            t1 * tangent.z + t2 * bitangent.z + remaining * stretched.z};
        return normalized(
            {alpha * hemisphere.x, alpha * hemisphere.y, (std::max) (0.0, hemisphere.z)}, {0.0, 0.0, 1.0});
    }

    // Independent binary64 integration of the reference isotropic VNDF/Smith
    // pair. No production helper participates in this oracle.
    [[nodiscard]] Double2 integrateBrdf(const double n_dot_v, const double roughness) noexcept {
        const double bounded_nv = (std::max) (n_dot_v, 1.0e-5);
        const double alpha = (std::max) (roughness, 0.001);
        const Double3 view{std::sqrt((std::max) (1.0 - bounded_nv * bounded_nv, 0.0)), 0.0, bounded_nv};

        double weighted = 0.0;
        double unweighted = 0.0;
        for (u32 index = 0u; index < kSampleCount; ++index) {
            const Double3 half_vector = vndfSample(hammersley(index), alpha, view);
            const double v_dot_h = (std::min) (1.0,
                (std::max) (0.0, view.x * half_vector.x + view.y * half_vector.y + view.z * half_vector.z));
            const Double3 light = normalized(
                {2.0 * v_dot_h * half_vector.x - view.x, 2.0 * v_dot_h * half_vector.y - view.y,
                    2.0 * v_dot_h * half_vector.z - view.z},
                half_vector);
            const double n_dot_l = light.z;
            if (!(n_dot_l > 0.0))
                continue;

            const double lambda_v = n_dot_l * std::sqrt(
                                                  alpha * alpha * view.x * view.x +
                                                  alpha * alpha * view.y * view.y +
                                                  bounded_nv * bounded_nv);
            const double lambda_l = bounded_nv * std::sqrt(
                                                     alpha * alpha * light.x * light.x +
                                                     alpha * alpha * light.y * light.y +
                                                     n_dot_l * n_dot_l);
            const double denominator = lambda_v + lambda_l;
            const double visibility = denominator > 1.0e-8 ? 0.5 / denominator : 0.0;
            const double g1_stretch = std::sqrt(
                alpha * alpha * view.x * view.x + alpha * alpha * view.y * view.y + bounded_nv * bounded_nv);
            const double g1_denominator = bounded_nv + g1_stretch;
            const double g1 = g1_denominator > 1.0e-8 ? 2.0 * bounded_nv / g1_denominator : 0.0;
            const double weight = g1 > 0.0 ? 4.0 * n_dot_l * visibility * bounded_nv / g1 : 0.0;
            const double fresnel = std::pow(1.0 - v_dot_h, 5.0);
            weighted += fresnel * weight;
            unweighted += weight;
        }

        const double inverse = 1.0 / static_cast<double>(kSampleCount);
        return {weighted * inverse, unweighted * inverse};
    }

    // ------------------------------------------------------------------
    // device fixture
    // ------------------------------------------------------------------
    // Independent CPU evaluation of the approved circular filters and visible
    // response, used only by the end-to-end frame-path assertions.
    [[nodiscard]] Double3 diffuseCpu(const lighting::CH &ch, const Double3 normal) noexcept {
        const Double3 mean{
            static_cast<double>(ch.m_coefficients[0].x) * kInvSqrtTwoPi,
            static_cast<double>(ch.m_coefficients[0].y) * kInvSqrtTwoPi,
            static_cast<double>(ch.m_coefficients[0].z) * kInvSqrtTwoPi};
        const double length_xy = std::sqrt(normal.x * normal.x + normal.y * normal.y);
        if (length_xy == 0.0) {
            return {kPi * mean.x, kPi * mean.y, kPi * mean.z};
        }

        const double theta = std::atan2(normal.y, normal.x);
        Double3 sum{};
        for (u32 order = 1u; order <= 4u; ++order) {
            const auto &cosine = ch.m_coefficients[2u * order - 1u];
            const auto &sine = ch.m_coefficients[2u * order];
            const double c = std::cos(static_cast<double>(order) * theta);
            const double s = std::sin(static_cast<double>(order) * theta);
            const double weight = 1.0 / (1.0 + static_cast<double>(order * order));
            sum.x += (static_cast<double>(cosine.x) * c + static_cast<double>(sine.x) * s) * kInvSqrtPi * weight;
            sum.y += (static_cast<double>(cosine.y) * c + static_cast<double>(sine.y) * s) * kInvSqrtPi * weight;
            sum.z += (static_cast<double>(cosine.z) * c + static_cast<double>(sine.z) * s) * kInvSqrtPi * weight;
        }

        return {
            kPi * (std::max) (0.0, mean.x + length_xy * sum.x), kPi * (std::max) (0.0, mean.y + length_xy * sum.y),
            kPi * (std::max) (0.0, mean.z + length_xy * sum.z)};
    }

    [[nodiscard]] Double3 specularCpu(
        const lighting::CH &ch, const Double3 reflection, const double roughness) noexcept {
        const Double3 mean{
            static_cast<double>(ch.m_coefficients[0].x) * kInvSqrtTwoPi,
            static_cast<double>(ch.m_coefficients[0].y) * kInvSqrtTwoPi,
            static_cast<double>(ch.m_coefficients[0].z) * kInvSqrtTwoPi};
        const double length_xy = std::sqrt(reflection.x * reflection.x + reflection.y * reflection.y);
        if (length_xy == 0.0)
            return mean;

        const double theta = std::atan2(reflection.y, reflection.x);
        Double3 sum{};
        for (u32 order = 1u; order <= 4u; ++order) {
            const auto &cosine = ch.m_coefficients[2u * order - 1u];
            const auto &sine = ch.m_coefficients[2u * order];
            const double c = std::cos(static_cast<double>(order) * theta);
            const double s = std::sin(static_cast<double>(order) * theta);
            const double weight = std::pow(length_xy, static_cast<double>(order)) *
                                  std::exp(-5.0 * static_cast<double>(order * order) * roughness * roughness);
            sum.x += (static_cast<double>(cosine.x) * c + static_cast<double>(sine.x) * s) * kInvSqrtPi * weight;
            sum.y += (static_cast<double>(cosine.y) * c + static_cast<double>(sine.y) * s) * kInvSqrtPi * weight;
            sum.z += (static_cast<double>(cosine.z) * c + static_cast<double>(sine.z) * s) * kInvSqrtPi * weight;
        }

        return {(std::max) (0.0, mean.x + sum.x), (std::max) (0.0, mean.y + sum.y), (std::max) (0.0, mean.z + sum.z)};
    }

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
        }

        void shutdown() {
            m_queue.setNull();
            PPR_TEST_ASSERT(not m_rhi->shutdown());
            PPR_TEST_ASSERT(not m_shaders->shutdown());
        }

        [[nodiscard]] rhi::IDevice &device() const {
            return m_rhi->getDevice();
        }

        [[nodiscard]] rhi::ComPtr<rhi::IShaderProgram> makeProgram(
            const std::filesystem::path &path, const char *const entry) {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(
                not m_shaders->loadModuleFromFile(path, "world_lighting_material_probe", module.writeRef()));
            shader::ComPtr<slang::IEntryPoint> point{};
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entry, point.writeRef())));
            slang::IComponentType *entries[]{point.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            rhi::ComPtr<rhi::IShaderProgram> program{};
            shader::Diagnose diagnostics;
            PPR_TEST_ASSERT(not make_error_code(
                device().createShaderProgram(description, program.writeRef(), diagnostics.writeRef())));
            return program;
        }

        void submit(rhi::ICommandEncoder &encoder) {
            rhi::ComPtr<rhi::ICommandBuffer> commands{};
            PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(m_queue->submit(commands)));
            PPR_TEST_ASSERT(not make_error_code(m_queue->waitOnHost()));
        }
    };

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeBuffer(
        rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial,
        const bool writable = false) {
        rhi::BufferDesc description{};
        description.size = std::max<u64>(bytes, stride);
        description.elementSize = stride;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        u32 usage = static_cast<u32>(rhi::BufferUsage::ShaderResource) | static_cast<u32>(rhi::BufferUsage::CopySource);
        if (writable)
            usage |= static_cast<u32>(rhi::BufferUsage::UnorderedAccess);
        description.usage = static_cast<rhi::BufferUsage>(usage);
        description.defaultState = writable ? rhi::ResourceState::UnorderedAccess : rhi::ResourceState::ShaderResource;
        rhi::ComPtr<rhi::IBuffer> buffer{};
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, initial, buffer.writeRef())));
        return buffer;
    }

    [[nodiscard]] rhi::ComPtr<rhi::ITextureView> defaultView(const rhi::ComPtr<rhi::ITexture> &texture) {
        rhi::ComPtr<rhi::ITextureView> view{};
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

    [[nodiscard]] std::array<float, 2u> lutTexel(const TextureReadback &readback, const u32 x, const u32 y) {
        const auto *const words = reinterpret_cast<const u16 *>(
            readback.m_bytes.data() + static_cast<std::size_t>(y) * readback.m_row_pitch + x * 4u);
        return {halfToFloat(words[0]), halfToFloat(words[1])};
    }

    [[nodiscard]] std::array<double, 2u> sampleLutCpu(
        const TextureReadback &readback, const double n_dot_v, const double roughness) {
        const double x = std::clamp(n_dot_v, 0.0, 1.0) * static_cast<double>(kWidth) - 0.5;
        const double y = std::clamp(roughness, 0.0, 1.0) * static_cast<double>(kHeight) - 0.5;
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const double fx = x - std::floor(x);
        const double fy = y - std::floor(y);
        const auto at = [&](const int ix, const int iy) {
            const u32 cx = static_cast<u32>(std::clamp(ix, 0, static_cast<int>(kWidth) - 1));
            const u32 cy = static_cast<u32>(std::clamp(iy, 0, static_cast<int>(kHeight) - 1));
            const auto value = lutTexel(readback, cx, cy);
            return std::array<double, 2u>{static_cast<double>(value[0]), static_cast<double>(value[1])};
        };

        const auto a = at(x0, y0);
        const auto b = at(x0 + 1, y0);
        const auto c = at(x0, y0 + 1);
        const auto d = at(x0 + 1, y0 + 1);
        return {
            (a[0] * (1.0 - fx) + b[0] * fx) * (1.0 - fy) + (c[0] * (1.0 - fx) + d[0] * fx) * fy,
            (a[1] * (1.0 - fx) + b[1] * fx) * (1.0 - fy) + (c[1] * (1.0 - fx) + d[1] * fx) * fy};
    }

    // ------------------------------------------------------------------
    // probe execution
    // ------------------------------------------------------------------

    struct ProbeCase {
        float4 m_base{};
        float4 m_parameters{}; // metallic, roughness, eta
        float4 m_direction{}; // normal or reflection
        float4 m_view{};
        float4 m_environment{};
        float4 m_authored{};
        float4 m_thermal{};
        float4 m_lookup{}; // NoV, roughness or fine position
        lighting::CH m_ch{};
    };

    [[nodiscard]] std::vector<float4> runProbe(
        Fixture &fixture, const char *const entry, const std::span<const ProbeCase> cases,
        rhi::ITextureView *const brdf_view = nullptr, rhi::IBuffer *const fine_ch_override = nullptr,
        std::vector<float4> *const detail_out = nullptr) {
        auto &device = fixture.device();
        rhi::ITextureView *table_view = brdf_view;
        Array<float4> fields(cases.size() * 8u);
        Array<lighting::PackedCH> packed(cases.size());
        for (u32 index = 0u; index < cases.size(); ++index) {
            const auto &item = cases[index];
            fields[index * 8u + 0u] = item.m_base;
            fields[index * 8u + 1u] = item.m_parameters;
            fields[index * 8u + 2u] = item.m_direction;
            fields[index * 8u + 3u] = item.m_view;
            fields[index * 8u + 4u] = item.m_environment;
            fields[index * 8u + 5u] = item.m_authored;
            fields[index * 8u + 6u] = item.m_thermal;
            fields[index * 8u + 7u] = item.m_lookup;
            const auto encoded = lighting::packCH(item.m_ch);
            PPR_TEST_ASSERT(encoded.has_value());
            packed[index] = *encoded;
        }

        auto fields_buffer = makeBuffer(device, fields.size() * sizeof(float4), sizeof(float4), fields.data());
        rhi::ComPtr<rhi::IBuffer> ch_buffer = makeBuffer(
            device, packed.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH), packed.data());
        auto output = makeBuffer(device, cases.size() * sizeof(float4), sizeof(float4), nullptr, true);
        auto detail = makeBuffer(device, cases.size() * 8u * sizeof(float4), sizeof(float4), nullptr, true);

        // The module declares the fine-CH buffer and BRDF table for every entry.
        // Bind zero-value defaults when the caller does not supply a real table.
        const std::array<u32, 8u> zero_words{};
        auto fine_default = makeBuffer(device, sizeof(zero_words), sizeof(u32), zero_words.data());
        rhi::ComPtr<rhi::ITexture> default_texture{};
        rhi::ComPtr<rhi::ITextureView> default_view{};
        if (brdf_view == nullptr) {
            const std::array<u16, 2u> zero_texel{};
            const rhi::SubresourceData initial{zero_texel.data(), 4u, 4u};
            rhi::TextureDesc texture_description{};
            texture_description.type = rhi::TextureType::Texture2D;
            texture_description.size = {.width = 1u, .height = 1u, .depth = 1u};
            texture_description.arrayLength = 1u;
            texture_description.mipCount = 1u;
            texture_description.format = rhi::Format::RG16Float;
            texture_description.memoryType = rhi::MemoryType::DeviceLocal;
            texture_description.usage = rhi::TextureUsage::ShaderResource;
            texture_description.defaultState = rhi::ResourceState::ShaderResource;
            PPR_TEST_ASSERT(
                not make_error_code(device.createTexture(texture_description, &initial, default_texture.writeRef())));
            default_view = defaultView(default_texture);
            table_view = default_view.get();
        }

        const auto path = std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_material_probe.slang";
        auto program = fixture.makeProgram(path, entry);
        rhi::ComPtr<rhi::IComputePipeline> pipeline{};
        rhi::ComputePipelineDesc pipeline_description{};
        pipeline_description.program = program.get();
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_description, pipeline.writeRef())));
        rhi::ComPtr<rhi::IShaderObject> root{};
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, root.writeRef())));

        struct alignas(16) ProbeFrame {
            u32 m_count{};
            u32 m_reserved[3]{};
        };
        ProbeFrame frame{};
        frame.m_count = static_cast<u32>(cases.size());
        rhi::ShaderCursor cursor{root.get()};
        rhi::ShaderCursor constants{};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_material_probe"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_fields"].setBinding(rhi::Binding(fields_buffer))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_ch"].setBinding(rhi::Binding(ch_buffer))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_output"].setBinding(rhi::Binding(output))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_detail"].setBinding(rhi::Binding(detail))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_fine_ch"].setBinding(
                rhi::Binding(fine_ch_override != nullptr ? fine_ch_override : fine_default.get()))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_brdf"].setBinding(rhi::Binding(table_view))));
        rhi::SamplerDesc sampling{};
        sampling.minFilter = rhi::TextureFilteringMode::Linear;
        sampling.magFilter = rhi::TextureFilteringMode::Linear;
        sampling.mipFilter = rhi::TextureFilteringMode::Linear;
        sampling.addressU = rhi::TextureAddressingMode::ClampToEdge;
        sampling.addressV = rhi::TextureAddressingMode::ClampToEdge;
        sampling.addressW = rhi::TextureAddressingMode::ClampToEdge;
        rhi::ComPtr<rhi::ISampler> sampler{};
        PPR_TEST_ASSERT(not make_error_code(device.createSampler(sampling, sampler.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_brdf_sampler"].setBinding(rhi::Binding(sampler))));

        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
        {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            PPR_DEFER {
                pass->end();
            };
            pass->bindPipeline(pipeline, root);
            pass->dispatchCompute((static_cast<u32>(cases.size()) + 63u) / 64u, 1u, 1u);
        }
        encoder->setBufferState(output, rhi::ResourceState::CopySource);
        encoder->setBufferState(detail, rhi::ResourceState::CopySource);
        fixture.submit(*encoder);

        std::vector<float4> result(cases.size());
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(output, 0u, result.size() * sizeof(float4), result.data())));
        if (detail_out != nullptr) {
            detail_out->resize(cases.size() * 8u);
            PPR_TEST_ASSERT(not make_error_code(
                device.readBuffer(detail, 0u, detail_out->size() * sizeof(float4), detail_out->data())));
        }

        return result;
    }

    [[nodiscard]] lighting::CH uniformCH(const float3 radiance) noexcept {
        lighting::CH ch{};
        ch.m_coefficients[0] = radiance / static_cast<float>(kInvSqrtTwoPi);
        return ch;
    }

    [[nodiscard]] lighting::CH directionalCH(const float3 mean, const float3 cosine) noexcept {
        lighting::CH ch{};
        ch.m_coefficients[0] = mean / static_cast<float>(kInvSqrtTwoPi);
        ch.m_coefficients[1] = cosine / static_cast<float>(kInvSqrtPi);
        return ch;
    }

    [[nodiscard]] bool close3(const float3 actual, const float3 expected, const float tolerance) noexcept {
        return std::abs(actual.x - expected.x) <= tolerance and
               std::abs(actual.y - expected.y) <= tolerance and
               std::abs(actual.z - expected.z) <= tolerance;
    }

    // ------------------------------------------------------------------
    // real grid_tiles sample-rate boundary capture
    // ------------------------------------------------------------------

    struct SampleQuad {
        std::array<std::array<float, 4u>, 4u> m_rgba{};
    };

    [[nodiscard]] SampleQuad renderResolvedCells(
        Fixture &fixture, Renderer &renderer, GridPass &grid, const std::span<const u32> resolved,
        const std::span<const GridTerrainVisual> visuals, const float3 ambient_radiance = float3{0.0f, 0.0f, 0.0f}) {
        PPR_TEST_ASSERT(resolved.size() == static_cast<std::size_t>(WorldLightingPass::kCellCount));

        GridStagedFrame stage{};
        stage.m_cell_size_m = 1.0f;
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_terrain_visuals.assign(visuals.begin(), visuals.end());
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

        auto &device = fixture.device();
        auto resolved_buffer = makeBuffer(device, resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        Array<lighting::PackedCH> zero_ch(lighting::kFineCHRecordCount);
        const auto uniform = lighting::packCH(uniformCH(ambient_radiance));
        PPR_TEST_ASSERT(uniform.has_value());
        for (auto &record: zero_ch) {
            record = *uniform;
        }
        auto fine_buffer = makeBuffer(
            device, zero_ch.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH), zero_ch.data());
        PPR_TEST_ASSERT(not grid.setLightingBuffersForTest(std::move(resolved_buffer), std::move(fine_buffer)));

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{-0.5f, 0.0f, -0.5f};
        model.m_ortho_scale = 1.0f;
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 4, 4}});
        const auto snapshot = camera.getSnapshot();
        const std::array<GridTileSubmission, 1u> planned_submissions{
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u}};
        const auto plan = GridPass::planTiles(snapshot, planned_submissions, 1.0f);
        PPR_TEST_ASSERT(plan.has_value());
        std::println("WORLD_LIGHTING_MATERIAL planned_tiles={}", plan->m_tiles.size());
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot, true));

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2DMS;
        target_description.size = {.width = 4u, .height = 4u, .depth = 1u};
        target_description.arrayLength = 1u;
        target_description.mipCount = 1u;
        target_description.sampleCount = 4u;
        target_description.format = rhi::Format::RGBA16Float;
        target_description.memoryType = rhi::MemoryType::DeviceLocal;
        target_description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        target_description.defaultState = rhi::ResourceState::RenderTarget;
        rhi::ComPtr<rhi::ITexture> target{};
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(target_description, nullptr, target.writeRef())));
        auto target_view = defaultView(target);

        const auto viewport = rhi::Viewport::fromSize(4.0f, 4.0f);
        const auto scissor = rhi::ScissorRect::fromSize(4u, 4u);
        const auto draw = [&](const DrawContext &context) -> std::error_code { return grid.render(context); };
        DrawSubmission submission{"grid material sample-rate", DrawCallback{draw}};
        submission.m_viewport = viewport;
        submission.m_scissor = scissor;
        PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {submission}));
        grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
        PPR_TEST_ASSERT(not renderer.waitOnHost());

        // Dump all four samples from the real MSAA target.
        shader::SharedModule dump_module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_backend.slang",
            "world_lighting_backend", dump_module.writeRef()));
        shader::ComPtr<slang::IEntryPoint> dump_entry{};
        PPR_TEST_ASSERT(not make_error_code(dump_module->findEntryPointByName("dumpSamples", dump_entry.writeRef())));
        slang::IComponentType *dump_entries[]{dump_entry.get()};
        rhi::ShaderProgramDesc dump_program_description{};
        dump_program_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        dump_program_description.slangGlobalScope = dump_module.get();
        dump_program_description.slangEntryPoints = dump_entries;
        dump_program_description.slangEntryPointCount = 1u;
        rhi::ComPtr<rhi::IShaderProgram> dump_program{};
        PPR_TEST_ASSERT(not make_error_code(
            device.createShaderProgram(dump_program_description, dump_program.writeRef(), nullptr)));
        rhi::ComPtr<rhi::IComputePipeline> dump_pipeline{};
        rhi::ComputePipelineDesc dump_pipeline_description{};
        dump_pipeline_description.program = dump_program.get();
        PPR_TEST_ASSERT(
            not make_error_code(device.createComputePipeline(dump_pipeline_description, dump_pipeline.writeRef())));

        rhi::TextureDesc dump_description = target_description;
        dump_description.type = rhi::TextureType::Texture2D;
        dump_description.size = {.width = 16u, .height = 4u, .depth = 1u};
        dump_description.sampleCount = 1u;
        dump_description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::ShaderResource) |
            static_cast<u32>(rhi::TextureUsage::UnorderedAccess) |
            static_cast<u32>(rhi::TextureUsage::CopySource));
        dump_description.defaultState = rhi::ResourceState::UnorderedAccess;
        rhi::ComPtr<rhi::ITexture> dump_texture{};
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(dump_description, nullptr, dump_texture.writeRef())));
        auto dump_view = defaultView(dump_texture);

        rhi::ComPtr<rhi::IShaderObject> dump_root{};
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(dump_program, dump_root.writeRef())));
        struct DumpParameters {
            u32 m_extent[2]{};
            u32 m_reserved[6]{};
        } parameters{};
        parameters.m_extent[0] = 4u;
        parameters.m_extent[1] = 4u;
        rhi::ShaderCursor dump_cursor{dump_root.get()};
        rhi::ShaderCursor dump_constants{};
        PPR_TEST_ASSERT(not make_error_code(dump_cursor["g_probe"].getDereferenced(dump_constants)));
        PPR_TEST_ASSERT(not make_error_code(dump_constants.setData(&parameters, sizeof(parameters))));
        PPR_TEST_ASSERT(not make_error_code(dump_cursor["g_msaa_color"].setBinding(rhi::Binding(target_view))));
        PPR_TEST_ASSERT(not make_error_code(dump_cursor["g_sample_dump"].setBinding(rhi::Binding(dump_view))));

        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        encoder->setTextureState(dump_texture, rhi::ResourceState::UnorderedAccess);
        {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            PPR_DEFER {
                pass->end();
            };
            pass->bindPipeline(dump_pipeline, dump_root);
            pass->dispatchCompute(1u, 1u, 1u);
        }
        encoder->setTextureState(dump_texture, rhi::ResourceState::CopySource);
        fixture.submit(*encoder);

        const auto readback = readTexture(device, *dump_texture);
        SampleQuad result{};
        for (u32 sample = 0u; sample < 4u; ++sample) {
            const auto *const words = reinterpret_cast<const u16 *>(
                readback.m_bytes.data() + static_cast<std::size_t>(1u) * readback.m_row_pitch + (4u + sample) * 8u);
            for (u32 channel = 0u; channel < 4u; ++channel) {
                result.m_rgba[sample][channel] = halfToFloat(words[channel]);
            }
        }

        return result;
    }

    // ------------------------------------------------------------------
    // categorical texture/normal probe
    // ------------------------------------------------------------------

    [[nodiscard]] image::ImageAsset makeConstantImage(const float4 rgba) noexcept {
        constexpr u32 size = 4u;
        Array<std::byte> pixels(size * size * 4u);
        for (u32 y = 0u; y < size; ++y) {
            for (u32 x = 0u; x < size; ++x) {
                const auto byte = [](const float value) {
                    return std::byte{static_cast<u8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f))};
                };
                const auto offset = (y * size + x) * 4u;
                pixels[offset + 0u] = byte(rgba.x);
                pixels[offset + 1u] = byte(rgba.y);
                pixels[offset + 2u] = byte(rgba.z);
                pixels[offset + 3u] = byte(rgba.w);
            }
        }

        image::ImageAsset image{};
        image.m_width = size;
        image.m_height = size;
        image.m_storage = mem::SharedBuffer::clone(
            mem::SharedBufferView{reinterpret_cast<const std::byte *>(pixels.data()), pixels.size()});
        image.m_subresources.push_back(
            image::ImageSubresource{
                .m_view = image.m_storage, .m_row_pitch = size * 4u, .m_slice_pitch = pixels.size()});
        PPR_TEST_ASSERT(
            not image::generateMipChain(image, image::MipGenDesc{.m_full_chain = true, .m_normal_map = false}));
        return image;
    }

    [[nodiscard]] float2 patchTranslationCpu(const int2 vertex) noexcept {
        const uint2 bounded{static_cast<u32>(vertex.x) & 0xffffu, static_cast<u32>(vertex.y) & 0xffffu};
        u32 value = bounded.x * 0x9e3779b9u ^ bounded.y * 0x85ebca6bu;
        value ^= value >> 16u;
        value *= 0x7feb352du;
        value ^= value >> 15u;
        value *= 0x846ca68bu;
        value ^= value >> 16u;
        return float2{static_cast<float>(value & 0xffffu) / 65536.0f, static_cast<float>(value >> 16u) / 65536.0f};
    }

    struct CpuPatches {
        float2 m_uv0{}, m_uv1{}, m_uv2{};
        float3 m_weights{};
    };

    [[nodiscard]] CpuPatches texturePatchesCpu(const float2 uv) noexcept {
        const float2 half_lattice{uv.x * 0.5f - uv.y * 0.288675135f, uv.y * 0.577350269f};
        const float2 lattice{
            (half_lattice.x - std::floor(half_lattice.x / 32768.0f) * 32768.0f) * 2.0f,
            (half_lattice.y - std::floor(half_lattice.y / 32768.0f) * 32768.0f) * 2.0f};
        const int2 cell{static_cast<int>(std::floor(lattice.x)), static_cast<int>(std::floor(lattice.y))};
        const float2 fraction{lattice.x - std::floor(lattice.x), lattice.y - std::floor(lattice.y)};
        int2 vertices[3] = {int2{}, int2{}, int2{}};
        CpuPatches result{};
        if (fraction.x + fraction.y <= 1.0f) {
            vertices[0] = cell;
            vertices[1] = int2{cell.x + 1, cell.y};
            vertices[2] = int2{cell.x, cell.y + 1};
            result.m_weights = float3{1.0f - fraction.x - fraction.y, fraction.x, fraction.y};
        } else {
            vertices[0] = int2{cell.x + 1, cell.y + 1};
            vertices[1] = int2{cell.x, cell.y + 1};
            vertices[2] = int2{cell.x + 1, cell.y};
            result.m_weights = float3{fraction.x + fraction.y - 1.0f, 1.0f - fraction.x, 1.0f - fraction.y};
        }

        const float2 repeat_uv{uv.x - std::floor(uv.x), uv.y - std::floor(uv.y)};
        result.m_uv0 = repeat_uv + patchTranslationCpu(vertices[0]);
        result.m_uv1 = repeat_uv + patchTranslationCpu(vertices[1]);
        result.m_uv2 = repeat_uv + patchTranslationCpu(vertices[2]);
        return result;
    }

    struct CategoricalCase {
        float2 m_fine{};
        float2 m_gradient_x{};
        float2 m_gradient_y{};
        float m_period{1.0f};
        float m_cell_size{1.0f};
        u32 m_material{1u};
    };

    [[nodiscard]] std::vector<float4> runCategoricalProbe(
        Fixture &fixture, const std::span<const CategoricalCase> cases, rhi::IBuffer *const palette,
        rhi::IBuffer *const descriptors, const rhi::DescriptorHandle sampler_handle) {
        auto &device = fixture.device();
        Array<float4> fields(cases.size() * 8u);
        for (u32 index = 0u; index < cases.size(); ++index) {
            const auto &item = cases[index];
            fields[index * 8u + 0u] = float4{
                item.m_gradient_x.x, item.m_gradient_x.y, item.m_gradient_y.x, item.m_gradient_y.y};
            fields[index * 8u + 1u] = float4{item.m_period, item.m_cell_size, 0.0f, 0.0f};
            fields[index * 8u + 2u] = float4{item.m_fine.x, item.m_fine.y, 0.0f, 0.0f};
            fields[index * 8u + 3u] = float4{static_cast<float>(item.m_material), 0.0f, 0.0f, 0.0f};
        }

        auto fields_buffer = makeBuffer(device, fields.size() * sizeof(float4), sizeof(float4), fields.data());
        auto detail = makeBuffer(device, cases.size() * 8u * sizeof(float4), sizeof(float4), nullptr, true);
        auto program = fixture.makeProgram(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_material_probe.slang",
            "probeCategoricalBase");
        rhi::ComPtr<rhi::IComputePipeline> pipeline{};
        rhi::ComputePipelineDesc pipeline_description{};
        pipeline_description.program = program.get();
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_description, pipeline.writeRef())));
        rhi::ComPtr<rhi::IShaderObject> root{};
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, root.writeRef())));

        struct alignas(16) ProbeFrame {
            u32 m_count{};
            u32 m_reserved[3]{};
        } frame{};
        frame.m_count = static_cast<u32>(cases.size());
        rhi::ShaderCursor cursor{root.get()};
        rhi::ShaderCursor constants{};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_material_probe"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_fields"].setBinding(rhi::Binding(fields_buffer))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_detail"].setBinding(rhi::Binding(detail))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_terrain_visuals"].setBinding(rhi::Binding(palette))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_textures"].setBinding(rhi::Binding(descriptors))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_sampler"].setDescriptorHandle(sampler_handle)));

        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        encoder->setBufferState(detail, rhi::ResourceState::UnorderedAccess);
        {
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            PPR_DEFER {
                pass->end();
            };
            pass->bindPipeline(pipeline, root);
            pass->dispatchCompute((static_cast<u32>(cases.size()) + 63u) / 64u, 1u, 1u);
        }
        encoder->setBufferState(detail, rhi::ResourceState::CopySource);
        fixture.submit(*encoder);

        std::vector<float4> result(cases.size() * 8u);
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(detail, 0u, result.size() * sizeof(float4), result.data())));
        return result;
    }
}

namespace pP::tests::detail::WorldLightingMaterial {
    // ------------------------------------------------------------------
    // immutable BRDF table
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (brdf_lut_matches_independent_binary64_integration) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid.shutdown());
        };

        rhi::ITexture *const table = grid.brdfLutForTest();
        PPR_TEST_ASSERT(table != nullptr);
        const auto &description = table->getDesc();
        PPR_TEST_ASSERT(description.size.width == kWidth);
        PPR_TEST_ASSERT(description.size.height == kHeight);
        PPR_TEST_ASSERT(description.format == rhi::Format::RG16Float);
        PPR_TEST_ASSERT(description.mipCount == 1u);
        PPR_TEST_ASSERT(description.sampleCount == 1u);
        const u64 bytes = static_cast<u64>(kWidth) * kHeight * 2u * sizeof(u16);
        PPR_TEST_ASSERT(bytes == 32768u);

        const auto readback = readTexture(fixture.device(), *table);
        PPR_TEST_ASSERT(readback.m_row_pitch >= static_cast<u64>(kWidth) * 4u);
        PPR_TEST_ASSERT(readback.m_bytes.size() >= static_cast<std::size_t>(kHeight) * readback.m_row_pitch);

        double maximum_error = 0.0;
        for (const u32 y: {0u, 1u, 63u, 127u}) {
            for (const u32 x: {0u, 1u, 31u, 62u, 63u}) {
                const auto actual = lutTexel(readback, x, y);
                const auto expected = integrateBrdf(
                    (static_cast<double>(x) + 0.5) / static_cast<double>(kWidth),
                    (static_cast<double>(y) + 0.5) / static_cast<double>(kHeight));
                const double tolerance_x = 0.004 + 0.003 * std::abs(expected.x);
                const double tolerance_y = 0.004 + 0.003 * std::abs(expected.y);
                maximum_error = (std::max) (maximum_error, std::abs(static_cast<double>(actual[0]) - expected.x));
                maximum_error = (std::max) (maximum_error, std::abs(static_cast<double>(actual[1]) - expected.y));
                PPR_TEST_ASSERT(
                    std::abs(static_cast<double>(actual[0]) - expected.x) <= tolerance_x and
                    std::abs(static_cast<double>(actual[1]) - expected.y) <= tolerance_y);
            }
        }

        std::println(
            "WORLD_LIGHTING_MATERIAL brdf_lut={}x{} rg16f bytes={} max_binary64_error={:.6f}", kWidth, kHeight, bytes,
            maximum_error);
        PPR_TEST_ASSERT(not grid.shutdown());
        PPR_TEST_ASSERT(grid.brdfLutForTest() == nullptr);
    };

    PPR_UNIT_TEST (brdf_lut_sampling_clamps_at_both_edges) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid.shutdown());
        };
        rhi::ITexture *const table = grid.brdfLutForTest();
        PPR_TEST_ASSERT(table != nullptr);
        rhi::ComPtr<rhi::ITextureView> view{};
        PPR_TEST_ASSERT(not make_error_code(table->getDefaultView(view.writeRef())));

        std::array<ProbeCase, 3u> cases{};
        cases[0].m_lookup = float4{-4.0f, -4.0f, 0.0f, 0.0f};
        cases[1].m_lookup = float4{0.0f, 0.0f, 0.0f, 0.0f};
        cases[2].m_lookup = float4{8.0f, 8.0f, 0.0f, 0.0f};
        const auto sampled = runProbe(fixture, "probeBrdf", cases, view.get());

        const auto readback = readTexture(fixture.device(), *table);
        const auto first = lutTexel(readback, 0u, 0u);
        const auto last = lutTexel(readback, kWidth - 1u, kHeight - 1u);
        PPR_TEST_ASSERT(close3(float3{sampled[0].x, sampled[0].y, 0.0f}, float3{first[0], first[1], 0.0f}, 1.0e-4f));
        PPR_TEST_ASSERT(close3(float3{sampled[1].x, sampled[1].y, 0.0f}, float3{first[0], first[1], 0.0f}, 1.0e-4f));
        PPR_TEST_ASSERT(close3(float3{sampled[2].x, sampled[2].y, 0.0f}, float3{last[0], last[1], 0.0f}, 1.0e-4f));
        std::println(
            "WORLD_LIGHTING_MATERIAL brdf_clamp low=({:.6f},{:.6f}) high=({:.6f},{:.6f})", float(sampled[0].x),
            float(sampled[0].y), float(sampled[2].x), float(sampled[2].y));
    };

    // ------------------------------------------------------------------
    // visible material response
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (perspective_and_orthographic_view_vectors_match_the_eye_contract) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 4u> cases{};
        // Orthographic: V is -forward regardless of the sample position.
        cases[0].m_base = float4{3.0f, 4.0f, 5.0f, 1.0f};
        cases[0].m_parameters = float4{0.0f, 0.0f, -1.0f, 1.0f};
        cases[0].m_direction = float4{100.0f, 100.0f, 0.0f, 0.0f};
        cases[0].m_view = float4{0.0f, 0.0f, 1.0f, 0.0f};
        // Perspective at the centre of the grid: eye = origin, V = +Z.
        cases[1].m_base = float4{0.0f, 0.0f, 5.0f, 1.0f};
        cases[1].m_parameters = float4{0.0f, 0.0f, -1.0f, 0.0f};
        cases[1].m_direction = float4{0.0f, 0.0f, 0.0f, 0.0f};
        cases[1].m_view = float4{0.0f, 0.0f, 1.0f, 0.0f};
        // Perspective off-centre: the eye point, not eye+forward, sets NoV.
        cases[2].m_base = float4{0.0f, 0.0f, 5.0f, 1.0f};
        cases[2].m_parameters = float4{0.0f, 0.0f, -1.0f, 0.0f};
        cases[2].m_direction = float4{1.0f, 1.0f, 0.0f, 0.0f};
        cases[2].m_view = float4{0.0f, 0.0f, 1.0f, 0.0f};
        // Orthographic with +Z forward: V = -Z, NoV clamps to 1e-5.
        cases[3].m_base = float4{0.0f, 0.0f, 5.0f, 1.0f};
        cases[3].m_parameters = float4{0.0f, 0.0f, 1.0f, 1.0f};
        cases[3].m_direction = float4{0.0f, 0.0f, 0.0f, 0.0f};
        cases[3].m_view = float4{0.0f, 0.0f, 1.0f, 0.0f};

        std::vector<float4> detail;
        const auto unused = runProbe(fixture, "probeView", cases, nullptr, nullptr, &detail);
        PPR_TEST_ASSERT(unused.size() == cases.size());
        PPR_TEST_ASSERT(detail.size() >= cases.size() * 2u);

        PPR_TEST_ASSERT(close3(float3{detail[0].x, detail[0].y, detail[0].z}, float3{0.0f, 0.0f, 1.0f}, 1.0e-5f));
        PPR_TEST_ASSERT(std::abs(float(detail[0].w) - 1.0f) <= 1.0e-5f);
        PPR_TEST_ASSERT(std::abs(float(detail[1].x) - 0.04f) <= 1.0e-5f);

        PPR_TEST_ASSERT(close3(float3{detail[2].x, detail[2].y, detail[2].z}, float3{0.0f, 0.0f, 1.0f}, 1.0e-5f));
        PPR_TEST_ASSERT(std::abs(float(detail[2].w) - 1.0f) <= 1.0e-5f);

        const float3 expected_off_centre = float3{-1.0f, -1.0f, 5.0f} / std::sqrt(27.0f);
        PPR_TEST_ASSERT(close3(float3{detail[4].x, detail[4].y, detail[4].z}, expected_off_centre, 1.0e-5f));
        const float expected_no_v = 5.0f / std::sqrt(27.0f);
        PPR_TEST_ASSERT(std::abs(float(detail[4].w) - expected_no_v) <= 1.0e-5f);
        PPR_TEST_ASSERT(std::abs(float(detail[4].w) - 4.0f / std::sqrt(18.0f)) > 0.01f);

        PPR_TEST_ASSERT(close3(float3{detail[6].x, detail[6].y, detail[6].z}, float3{0.0f, 0.0f, -1.0f}, 1.0e-5f));
        PPR_TEST_ASSERT(std::abs(float(detail[6].w) - 1.0e-5f) <= 1.0e-7f);

        std::println(
            "WORLD_LIGHTING_MATERIAL view ortho=({:.4f},{:.4f},{:.4f}) centre=({:.4f},{:.4f},{:.4f}) "
            "offcentre=({:.4f},{:.4f},{:.4f}) noV={:.6f}",
            float(detail[0].x), float(detail[0].y), float(detail[0].z), float(detail[2].x), float(detail[2].y),
            float(detail[2].z), float(detail[4].x), float(detail[4].y), float(detail[4].z), float(detail[4].w));
    };

    PPR_UNIT_TEST (black_environment_and_no_emission_is_black) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 1u> cases{};
        cases[0].m_base = float4{0.7f, 0.4f, 0.2f, 0.0f};
        cases[0].m_parameters = float4{0.2f, 0.5f, 1.5f, 0.0f};
        cases[0].m_direction = float4{0.3f, 0.2f, 0.93f, 0.0f};
        cases[0].m_view = float4{0.0f, 0.0f, -1.0f, 0.0f};
        cases[0].m_environment = float4{0.0f, 0.0f, 0.0f, 0.0f};
        cases[0].m_ch = lighting::CH{};
        const auto result = runProbe(fixture, "probeResponse", cases);
        PPR_TEST_ASSERT(close3(float3{result[0].x, result[0].y, result[0].z}, float3{0.0f, 0.0f, 0.0f}, 1.0e-6f));
    };

    PPR_UNIT_TEST (doubling_environment_doubles_reflected_hdr) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 2u> cases{};
        for (u32 index = 0u; index < cases.size(); ++index) {
            cases[index].m_base = float4{0.6f, 0.5f, 0.3f, 0.0f};
            cases[index].m_parameters = float4{0.1f, 0.4f, 1.45f, 0.0f};
            cases[index].m_direction = float4{0.2f, -0.3f, 0.9327f, 0.0f};
            cases[index].m_view = float4{0.1f, 0.2f, -0.9747f, 0.0f};
            cases[index].m_environment = float4{0.4f, 0.8f, 0.0f, 0.0f};
            cases[index].m_ch = directionalCH(float3{0.5f, 0.7f, 1.1f}, float3{0.2f, -0.1f, 0.15f});
            if (index == 1u) {
                for (auto &coefficient: cases[index].m_ch.m_coefficients)
                    coefficient *= 2.0f;
            }
        }

        const auto result = runProbe(fixture, "probeResponse", cases);
        const float3 single{result[0].x, result[0].y, result[0].z};
        const float3 doubled{result[1].x, result[1].y, result[1].z};
        PPR_TEST_ASSERT(close3(doubled, single * 2.0f, (std::max) (1.0e-5f, 2.0e-4f * doubled.z)));
    };

    PPR_UNIT_TEST (metallic_and_roughness_endpoints_change_visible_response) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 4u> cases{};
        for (u32 index = 0u; index < cases.size(); ++index) {
            cases[index].m_base = float4{0.55f, 0.45f, 0.25f, 0.0f};
            cases[index].m_parameters = float4{0.0f, 0.2f, 1.5f, 0.0f};
            cases[index].m_direction = float4{0.15f, 0.1f, 0.9836f, 0.0f};
            cases[index].m_view = float4{0.0f, 0.0f, -1.0f, 0.0f};
            cases[index].m_environment = float4{0.5f, 0.9f, 0.0f, 0.0f};
            cases[index].m_ch = uniformCH(float3{0.8f, 0.7f, 0.6f});
        }

        cases[1].m_parameters.x = 1.0f;
        cases[2].m_parameters.y = 0.8f;
        cases[3].m_parameters.y = 0.2f;
        const auto result = runProbe(fixture, "probeResponse", cases);
        const float3 dielectric{result[0].x, result[0].y, result[0].z};
        const float3 metal{result[1].x, result[1].y, result[1].z};
        const float3 rough{result[2].x, result[2].y, result[2].z};
        const float3 smooth{result[3].x, result[3].y, result[3].z};
        PPR_TEST_ASSERT(not close3(dielectric, metal, 1.0e-3f));
        PPR_TEST_ASSERT(not close3(rough, smooth, 1.0e-3f));
        PPR_TEST_ASSERT(metal.z > 0.0f and smooth.z > 0.0f);
    };

    PPR_UNIT_TEST (authored_albedo_and_normal_change_visible_response) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 3u> cases{};
        for (auto &item: cases) {
            item.m_base = float4{0.65f, 0.35f, 0.2f, 0.0f};
            item.m_parameters = float4{0.0f, 0.4f, 1.5f, 0.0f};
            item.m_direction = float4{0.0f, 0.0f, 1.0f, 0.0f};
            item.m_view = float4{0.0f, 0.0f, -1.0f, 0.0f};
            item.m_environment = float4{0.45f, 0.85f, 0.0f, 0.0f};
            item.m_ch = directionalCH(float3{0.5f, 0.55f, 0.6f}, float3{0.2f, 0.05f, -0.1f});
        }

        cases[1].m_base = float4{0.15f, 0.85f, 0.4f, 0.0f};
        cases[2].m_direction = float4{0.5f, -0.5f, 0.7071f, 0.0f};
        const auto result = runProbe(fixture, "probeResponse", cases);
        const float3 first{result[0].x, result[0].y, result[0].z};
        const float3 albedo_changed{result[1].x, result[1].y, result[1].z};
        const float3 normal_changed{result[2].x, result[2].y, result[2].z};
        PPR_TEST_ASSERT(not close3(first, albedo_changed, 1.0e-3f));
        PPR_TEST_ASSERT(not close3(first, normal_changed, 1.0e-3f));
    };

    PPR_UNIT_TEST (uniform_environment_is_independent_of_ch_direction) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 4u> cases{};
        const std::array<float4, 4u> normals{{
            float4{1.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 1.0f, 0.0f, 0.0f},
            float4{0.7071f, 0.7071f, 0.0f, 0.0f},
            float4{0.6f, -0.8f, 0.0f, 0.0f},
        }};
        for (u32 index = 0u; index < cases.size(); ++index) {
            cases[index].m_direction = normals[index];
            cases[index].m_ch = uniformCH(float3{0.45f, 0.6f, 0.9f});
        }

        const auto diffuse = runProbe(fixture, "probeDiffuse", cases);
        const auto specular = runProbe(fixture, "probeSpecular", cases);
        for (u32 index = 1u; index < cases.size(); ++index) {
            PPR_TEST_ASSERT(close3(
                float3{diffuse[index].x, diffuse[index].y, diffuse[index].z},
                float3{diffuse[0].x, diffuse[0].y, diffuse[0].z}, 1.0e-5f));
            PPR_TEST_ASSERT(close3(
                float3{specular[index].x, specular[index].y, specular[index].z},
                float3{specular[0].x, specular[0].y, specular[0].z}, 1.0e-5f));
        }

        const float3 expected_diffuse = float3{0.45f, 0.6f, 0.9f} * static_cast<float>(kPi);
        const float3 expected_specular = float3{0.45f, 0.6f, 0.9f};
        std::println(
            "WORLD_LIGHTING_MATERIAL uniform_diffuse=({:.6f},{:.6f},{:.6f}) specular=({:.6f},{:.6f},{:.6f})",
            float(diffuse[0].x), float(diffuse[0].y), float(diffuse[0].z), float(specular[0].x), float(specular[0].y),
            float(specular[0].z));
        PPR_TEST_ASSERT(close3(float3{diffuse[0].x, diffuse[0].y, diffuse[0].z}, expected_diffuse, 0.01f));
        PPR_TEST_ASSERT(close3(float3{specular[0].x, specular[0].y, specular[0].z}, expected_specular, 0.01f));
    };

    PPR_UNIT_TEST (zero_xy_normal_and_reflection_are_continuous) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 2u> cases{};
        for (u32 index = 0u; index < cases.size(); ++index) {
            cases[index].m_direction = index == 0u ? float4{0.0f, 0.0f, 1.0f, 0.0f} : float4{1.0e-5f, 0.0f, 1.0f, 0.0f};
            cases[index].m_parameters = float4{0.0f, 0.35f, 1.5f, 0.0f};
            cases[index].m_ch = directionalCH(float3{0.3f, 0.4f, 0.6f}, float3{0.1f, -0.2f, 0.05f});
        }

        const auto diffuse = runProbe(fixture, "probeDiffuse", cases);
        const auto specular = runProbe(fixture, "probeSpecular", cases);
        PPR_TEST_ASSERT(close3(
            float3{diffuse[0].x, diffuse[0].y, diffuse[0].z}, float3{diffuse[1].x, diffuse[1].y, diffuse[1].z},
            2.0e-3f));
        PPR_TEST_ASSERT(close3(
            float3{specular[0].x, specular[0].y, specular[0].z}, float3{specular[1].x, specular[1].y, specular[1].z},
            2.0e-3f));
    };

    PPR_UNIT_TEST (authored_emission_is_added_once_and_thermal_matches_source) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        std::array<ProbeCase, 2u> cases{};
        for (auto &item: cases) {
            item.m_base = float4{0.0f, 0.0f, 0.0f, 0.0f};
            item.m_parameters = float4{0.0f, 1.0f, 1.0f, 0.0f};
            item.m_direction = float4{0.0f, 0.0f, 1.0f, 0.0f};
            item.m_view = float4{0.0f, 0.0f, -1.0f, 0.0f};
            item.m_environment = float4{0.0f, 0.0f, 0.0f, 0.0f};
            item.m_ch = lighting::CH{};
        }
        cases[0].m_authored = float4{0.2f, 0.1f, 0.05f, 0.0f};
        cases[0].m_thermal = float4{1.5f, 0.75f, 0.25f, 0.0f};
        cases[1].m_authored = float4{0.4f, 0.2f, 0.1f, 0.0f};
        cases[1].m_thermal = float4{3.0f, 1.5f, 0.5f, 0.0f};
        const auto result = runProbe(fixture, "probeResponse", cases);
        PPR_TEST_ASSERT(close3(
            float3{result[1].x, result[1].y, result[1].z}, float3{result[0].x, result[0].y, result[0].z} * 2.0f,
            1.0e-5f));
    };

    PPR_UNIT_TEST (categorical_base_color_normal_bombing_and_period_contracts) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        const auto red = makeConstantImage(float4{1.0f, 0.0f, 0.0f, 1.0f});
        const auto green = makeConstantImage(float4{0.0f, 1.0f, 0.0f, 1.0f});
        const auto normal_x = makeConstantImage(float4{0.725f, 0.5f, 0.9465f, 1.0f});
        const auto normal_y = makeConstantImage(float4{0.5f, 0.725f, 0.9465f, 1.0f});
        const auto normal_neg_x = makeConstantImage(float4{0.275f, 0.5f, 0.9465f, 1.0f});

        const auto visual = [](const u32 id, const u32 texture, const u32 normal, const float period) {
            GridTerrainVisual value{};
            value.m_material_id = id;
            value.m_color_rgba = 0xffff'ffffu;
            value.m_texture_index = texture;
            value.m_texture_period_m = period;
            value.m_normal_texture_index = normal;
            value.m_normal_scale = 2.0f;
            value.m_ior = 1.5f;
            value.m_roughness = 0.4f;
            value.m_opacity = 1.0f;
            return value;
        };

        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_content_fingerprint = 7u;
        stage.m_terrain_images = {red, green, normal_x, normal_y, normal_neg_x};
        stage.m_terrain_visuals = {
            visual(1u, 0u, 0xffff'ffffu, 1.0f),
            visual(2u, 1u, 0xffff'ffffu, 1.0f),
            visual(3u, 0u, 2u, 1.0f),
            visual(4u, 0u, 3u, 1.0f),
            visual(5u, 0u, 4u, 1.0f),
            visual(6u, 0u, 0xffff'ffffu, 2.0f),
        };
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});

        PPR_TEST_ASSERT(not grid.stageWorldCandidate(7u, 7u, stage));
        bool prepared = false;
        for (u32 step = 0u; step < 32u and not prepared; ++step) {
            const auto progress = grid.advanceWorldCandidate(fixture.device(), renderer.completedFenceValue(), 8u);
            PPR_TEST_ASSERT(progress.has_value());
            prepared = *progress;
        }

        PPR_TEST_ASSERT(prepared);
        PPR_TEST_ASSERT(not grid.activateWorldCandidate(7u, 7u, 7u));

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{-0.5f, 0.0f, -0.5f};
        model.m_ortho_scale = 1.0f;
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 4, 4}});
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot(), false));

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 4u, .height = 4u, .depth = 1u};
        target_description.arrayLength = 1u;
        target_description.mipCount = 1u;
        target_description.format = rhi::Format::RGBA16Float;
        target_description.memoryType = rhi::MemoryType::DeviceLocal;
        target_description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        target_description.defaultState = rhi::ResourceState::RenderTarget;
        rhi::ComPtr<rhi::ITexture> target{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(target_description, nullptr, target.writeRef())));
        const auto viewport = rhi::Viewport::fromSize(4.0f, 4.0f);
        const auto scissor = rhi::ScissorRect::fromSize(4u, 4u);
        const auto draw = [&](const DrawContext &context) -> std::error_code { return grid.render(context); };
        DrawSubmission submission{"grid categorical palette", DrawCallback{draw}};
        submission.m_viewport = viewport;
        submission.m_scissor = scissor;
        PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {submission}));
        grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
        PPR_TEST_ASSERT(not renderer.waitOnHost());

        auto *const palette = grid.terrainPaletteForTest();
        auto *const descriptors = grid.textureDescriptorsForTest();
        PPR_TEST_ASSERT(palette != nullptr and descriptors != nullptr);

        const auto make_case = [](const float2 fine, const float2 gradient_x, const float2 gradient_y,
                                   const float period, const u32 material) {
            CategoricalCase value{};
            value.m_fine = fine;
            value.m_gradient_x = gradient_x;
            value.m_gradient_y = gradient_y;
            value.m_period = period;
            value.m_cell_size = 1.0f;
            value.m_material = material;
            return value;
        };
        const std::array<CategoricalCase, 7u> cases{
            make_case(float2{0.25f, 0.25f}, float2{0.01f, 0.02f}, float2{0.03f, 0.04f}, 1.0f, 1u),
            make_case(float2{0.75f, 0.75f}, float2{0.01f, 0.02f}, float2{0.03f, 0.04f}, 1.0f, 1u),
            make_case(float2{0.25f, 0.25f}, float2{0.01f, 0.02f}, float2{0.03f, 0.04f}, 1.0f, 2u),
            make_case(float2{0.25f, 0.25f}, float2{0.01f, 0.02f}, float2{0.03f, 0.04f}, 1.0f, 3u),
            make_case(float2{0.25f, 0.25f}, float2{0.01f, 0.02f}, float2{0.03f, 0.04f}, 1.0f, 4u),
            make_case(float2{0.25f, 0.25f}, float2{0.01f, 0.02f}, float2{0.03f, 0.04f}, 1.0f, 5u),
            make_case(float2{0.25f, 0.25f}, float2{0.01f, 0.02f}, float2{0.03f, 0.04f}, 2.0f, 6u),
        };

        const auto detail = runCategoricalProbe(
            fixture, cases, palette, descriptors, grid.terrainSamplerHandleForTest());
        PPR_TEST_ASSERT(detail.size() == cases.size() * 8u);
        const auto field = [&](const u32 index, const u32 slot) { return detail[index * 8u + slot]; };

        const auto base0 = field(0u, 0u);
        const auto raw0 = field(0u, 1u);
        const auto weights0 = field(0u, 2u);
        const auto mean0 = field(0u, 7u);
        PPR_TEST_ASSERT(float(base0.x) > 0.8f and float(base0.y) < 0.1f and float(base0.z) < 0.1f);
        PPR_TEST_ASSERT(float(field(2u, 0u).y) > 0.8f and float(field(2u, 0u).x) < 0.1f);

        const float3 raw_rgb{raw0.x, raw0.y, raw0.z};
        const float3 mean_rgb{mean0.x, mean0.y, mean0.z};
        const float3 weights{weights0.x, weights0.y, weights0.z};
        const float3 expected = float3{
            std::clamp(mean_rgb.x + (raw_rgb.x - mean_rgb.x) / std::sqrt(dot(weights, weights)), 0.0f, 1.0f),
            std::clamp(mean_rgb.y + (raw_rgb.y - mean_rgb.y) / std::sqrt(dot(weights, weights)), 0.0f, 1.0f),
            std::clamp(mean_rgb.z + (raw_rgb.z - mean_rgb.z) / std::sqrt(dot(weights, weights)), 0.0f, 1.0f)};
        PPR_TEST_ASSERT(close3(float3{base0.x, base0.y, base0.z}, expected, 1.0e-4f));

        const auto cpu_patches = texturePatchesCpu(float2{0.25f, 0.25f});
        const auto uv01 = field(0u, 3u);
        const auto uv2_normal = field(0u, 4u);
        PPR_TEST_ASSERT(
            close3(float3{uv01.x, uv01.y, 0.0f}, float3{cpu_patches.m_uv0.x, cpu_patches.m_uv0.y, 0.0f}, 1.0e-5f));
        PPR_TEST_ASSERT(
            close3(float3{uv01.z, uv01.w, 0.0f}, float3{cpu_patches.m_uv1.x, cpu_patches.m_uv1.y, 0.0f}, 1.0e-5f));
        PPR_TEST_ASSERT(close3(
            float3{uv2_normal.x, uv2_normal.y, 0.0f}, float3{cpu_patches.m_uv2.x, cpu_patches.m_uv2.y, 0.0f}, 1.0e-5f));
        PPR_TEST_ASSERT(close3(
            float3{weights0.x, weights0.y, weights0.z},
            float3{cpu_patches.m_weights.x, cpu_patches.m_weights.y, cpu_patches.m_weights.z}, 1.0e-5f));
        PPR_TEST_ASSERT(
            std::abs(float(field(1u, 3u).x) - float(field(0u, 3u).x)) > 1.0e-3f or
            std::abs(float(field(1u, 3u).y) - float(field(0u, 3u).y)) > 1.0e-3f);

        const auto normal_x_result = field(3u, 4u);
        const auto normal_y_result = field(4u, 4u);
        const auto normal_neg_x_result = field(5u, 4u);
        PPR_TEST_ASSERT(float(normal_x_result.z) > 0.3f and std::abs(float(normal_x_result.w)) < 0.1f);
        PPR_TEST_ASSERT(float(normal_y_result.w) > 0.3f and std::abs(float(normal_y_result.z)) < 0.1f);
        PPR_TEST_ASSERT(float(normal_neg_x_result.z) < -0.3f);

        const float expected_gradient = 0.01f / 1.0f;
        std::println(
            "WORLD_LIGHTING_MATERIAL categorical detail5=({:.6f},{:.6f},{:.6f},{:.6f}) detail6=({:.6f},{:.6f},"
            "{:.6f},{:.6f})",
            float(field(0u, 5u).x), float(field(0u, 5u).y), float(field(0u, 5u).z), float(field(0u, 5u).w),
            float(field(0u, 6u).x), float(field(0u, 6u).y), float(field(0u, 6u).z), float(field(0u, 6u).w));
        PPR_TEST_ASSERT(std::abs(float(field(0u, 5u).y) - expected_gradient) <= 1.0e-6f);
        PPR_TEST_ASSERT(std::abs(float(field(0u, 5u).z) - 0.02f) <= 1.0e-6f);
        PPR_TEST_ASSERT(std::abs(float(field(6u, 5u).y) - 0.01f / 2.0f) <= 1.0e-6f);

        std::println(
            "WORLD_LIGHTING_MATERIAL categorical base=({:.4f},{:.4f},{:.4f}) normal_x={:.4f} normal_y={:.4f} "
            "uv_gradient={:.6f} period2_gradient={:.6f}",
            float(base0.x), float(base0.y), float(base0.z), float(normal_x_result.z), float(normal_y_result.w),
            float(field(0u, 5u).y), float(field(6u, 5u).y));
    };

    PPR_UNIT_TEST (gas_uses_nonzero_ch_angular_mean_and_alpha_composition) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        GridTerrainVisual gas_visual{};
        gas_visual.m_material_id = 1u;
        gas_visual.m_color_rgba = 0x00ff00ffu;
        gas_visual.m_ior = 1.5f;
        gas_visual.m_roughness = 0.5f;
        gas_visual.m_opacity = 1.0f;

        std::array<GridParticleMetadataRow, 1u> rows{};
        rows[0].m_id_low = 1u;
        rows[0].m_source_row = 0xffff'ffffu;
        rows[0].m_gas_x = 1.5f;
        rows[0].m_gas_y = 2.5f;
        rows[0].m_radius = 1.0f;
        const auto material_temperature = lighting::packCell(1u, 0.0);
        PPR_TEST_ASSERT(material_temperature.has_value());
        rows[0].m_material_temperature = *material_temperature;
        const auto metadata = prepareGridParticleMetadata(fixture.device(), rows, 0u, 1.0, 1u);
        PPR_TEST_ASSERT(metadata.has_value());

        GridParticleGpuView view{};
        view.m_numerical = makeBuffer(
            fixture.device(), kGridParticleNumericalStride, kGridParticleNumericalStride, nullptr);
        view.m_readonly = makeBuffer(
            fixture.device(), kGridParticleReadonlyStride, kGridParticleReadonlyStride, nullptr);
        view.m_metadata = *metadata;
        view.m_live_epoch = std::make_shared<std::atomic<u64>>(1u);
        view.m_world = grid.activeWorldIdentity();
        view.m_layout = kGridParticleNumericalLayout;
        view.m_stride = kGridParticleNumericalStride;
        view.m_readonly_stride = kGridParticleReadonlyStride;
        view.m_origin = 3.0;
        view.m_device_epoch = 1u;
        view.m_publication = 1u;
        view.m_terrain_revision = 1u;
        view.m_joint_tick = 1u;

        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals = {gas_visual};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        stage.m_gpu_particles = view;
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        Array<u32> resolved(static_cast<std::size_t>(WorldLightingPass::kCellCount), 0u);
        auto resolved_buffer = makeBuffer(
            fixture.device(), resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        const auto make_ch = [&](const float3 radiance) {
            Array<lighting::PackedCH> records(lighting::kFineCHRecordCount);
            const auto packed = lighting::packCH(uniformCH(radiance));
            PPR_TEST_ASSERT(packed.has_value());
            for (auto &record: records)
                record = *packed;
            return makeBuffer(
                fixture.device(), records.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH),
                records.data());
        };

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{-0.5f, 0.0f, -0.5f};
        model.m_ortho_scale = 1.0f;
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 4, 4}});
        const auto snapshot = camera.getSnapshot();

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 4u, .height = 4u, .depth = 1u};
        target_description.arrayLength = 1u;
        target_description.mipCount = 1u;
        target_description.format = rhi::Format::RGBA16Float;
        target_description.memoryType = rhi::MemoryType::DeviceLocal;
        target_description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
        target_description.defaultState = rhi::ResourceState::RenderTarget;
        rhi::ComPtr<rhi::ITexture> target{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(target_description, nullptr, target.writeRef())));
        const auto viewport = rhi::Viewport::fromSize(4.0f, 4.0f);
        const auto scissor = rhi::ScissorRect::fromSize(4u, 4u);

        const auto capture = [&](rhi::ComPtr<rhi::IBuffer> fine_ch) {
            PPR_TEST_ASSERT(not grid.setLightingBuffersForTest(resolved_buffer, std::move(fine_ch)));
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot, false));
            const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                return grid.prepareGpuParticles(context);
            };
            const auto draw = [&](const DrawContext &context) -> std::error_code {
                if (const auto ec = grid.render(context))
                    return ec;
                return grid.renderParticles(context);
            };
            DrawSubmission submission{"grid gas CH", DrawCallback{draw}};
            submission.m_viewport = viewport;
            submission.m_scissor = scissor;
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {submission}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const auto readback = readTexture(fixture.device(), *target);
            const auto *const words = reinterpret_cast<const u16 *>(
                readback.m_bytes.data() + static_cast<std::size_t>(1u) * readback.m_row_pitch + 1u * 8u);
            std::array<float, 4u> pixel{};
            for (u32 channel = 0u; channel < 4u; ++channel)
                pixel[channel] = halfToFloat(words[channel]);
            return pixel;
        };

        const auto lit = capture(make_ch(float3{0.5f, 0.25f, 0.75f}));
        const auto dark = capture(make_ch(float3{0.0f, 0.0f, 0.0f}));
        // The cold-start render is exactly the clear color attenuated by the gas
        // opacity, so it recovers alpha without guessing the mask's mip level.
        const float alpha = 1.0f - dark[0] / 0.1f;
        const std::array<float, 3u> expected{
            0.1f * (1.0f - alpha), 0.25f * alpha + 0.1f * (1.0f - alpha), 0.2f * (1.0f - alpha)};
        PPR_TEST_ASSERT(alpha > 0.5f and alpha < 0.9f);
        PPR_TEST_ASSERT(std::abs(lit[0] - expected[0]) <= 0.01f);
        PPR_TEST_ASSERT(std::abs(lit[1] - expected[1]) <= 0.01f);
        PPR_TEST_ASSERT(std::abs(lit[2] - expected[2]) <= 0.01f);
        PPR_TEST_ASSERT(lit[3] > 0.9f);
        PPR_TEST_ASSERT(dark[3] > 0.9f);
        PPR_TEST_ASSERT(lit[1] > dark[1] + 0.1f);
        std::println(
            "WORLD_LIGHTING_MATERIAL gas lit=({:.4f},{:.4f},{:.4f},{:.4f}) dark=({:.4f},{:.4f},{:.4f},{:.4f}) "
            "alpha={:.4f} instances={} draws={}",
            lit[0], lit[1], lit[2], lit[3], dark[0], dark[1], dark[2], dark[3], alpha,
            grid.observations().m_particle_instances, grid.observations().m_draw_commands);
    };

    PPR_UNIT_TEST (categorical_view_vector_matches_the_literal_grid_frame_path) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        GridTerrainVisual visual{};
        visual.m_material_id = 1u;
        visual.m_color_rgba = 0xffffffffu;
        visual.m_ior = 1.5f;
        visual.m_roughness = 0.5f;
        visual.m_metallic = 0.0f;
        visual.m_opacity = 1.0f;
        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals = {visual};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

        Array<u32> resolved(static_cast<std::size_t>(WorldLightingPass::kCellCount), 1u);
        auto resolved_buffer = makeBuffer(
            fixture.device(), resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        const auto make_ch = [&](const lighting::CH &ch) {
            Array<lighting::PackedCH> records(lighting::kFineCHRecordCount);
            const auto packed = lighting::packCH(ch);
            PPR_TEST_ASSERT(packed.has_value());
            for (auto &record: records)
                record = *packed;
            return makeBuffer(
                fixture.device(), records.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH),
                records.data());
        };

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 1u, .height = 1u, .depth = 1u};
        target_description.arrayLength = 1u;
        target_description.mipCount = 1u;
        target_description.format = rhi::Format::RGBA16Float;
        target_description.memoryType = rhi::MemoryType::DeviceLocal;
        target_description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
        target_description.defaultState = rhi::ResourceState::RenderTarget;
        rhi::ComPtr<rhi::ITexture> target{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(target_description, nullptr, target.writeRef())));
        const auto viewport = rhi::Viewport::fromSize(1.0f, 1.0f);
        const auto scissor = rhi::ScissorRect::fromSize(1u, 1u);

        const auto render_pixel = [&](const CameraSnapshot &snapshot, const lighting::CH &ch) {
            PPR_TEST_ASSERT(not grid.setLightingBuffersForTest(resolved_buffer, make_ch(ch)));
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot, false));
            const auto draw = [&](const DrawContext &context) -> std::error_code { return grid.render(context); };
            DrawSubmission submission{"categorical view", DrawCallback{draw}};
            submission.m_viewport = viewport;
            submission.m_scissor = scissor;
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {submission}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const auto readback = readTexture(fixture.device(), *target);
            const auto *const words = reinterpret_cast<const u16 *>(readback.m_bytes.data());
            std::array<double, 4u> result{};
            for (u32 channel = 0u; channel < 4u; ++channel)
                result[channel] = static_cast<double>(halfToFloat(words[channel]));
            return result;
        };

        // --------------------------------------------------------------
        // orthographic default camera: V = -forward, flat normal NoV = 1e-5
        // --------------------------------------------------------------
        CameraModel ortho_model{};
        ortho_model.m_camera_mode = ECameraProjection::orthographic;
        ortho_model.m_origin = float3{0.0f, 0.0f, -0.5f};
        ortho_model.m_ortho_scale = 1.0f;
        Camera ortho_camera{ECameraProjection::orthographic};
        ortho_camera.updateModel(TimeSpan{}, ortho_model, Viewport{PixelRect{0, 0, 1, 1}});
        const auto ortho_snapshot = ortho_camera.getSnapshot();
        const double ortho_n_dot_v = std::max(static_cast<double>(ortho_snapshot.m_forward.z) * -1.0, 1.0e-5);
        const lighting::CH ortho_ch = uniformCH(float3{0.5f, 0.5f, 0.5f});
        const auto ortho_pixel = render_pixel(ortho_snapshot, ortho_ch);
        const auto lut = readTexture(fixture.device(), *grid.brdfLutForTest());
        const auto literal_pair = sampleLutCpu(lut, ortho_n_dot_v, 0.5);
        const double f0 = 0.04;
        const double fresnel = f0 + (std::max(1.0 - 0.5, f0) - f0) * std::pow(1.0 - ortho_n_dot_v, 5.0);
        const double energy = 1.0 + f0 * (1.0 / std::max(literal_pair[1], 1.0e-5) - 1.0);
        const double k_s = std::max(0.0, (literal_pair[0] * (1.0 - f0) + literal_pair[1] * f0) * energy);
        const double literal_channels = (1.0 - fresnel) * 0.5 + k_s * 0.5;
        const auto wrong_pair = sampleLutCpu(lut, 1.0, 0.5);
        const double wrong_fresnel = f0;
        const double wrong_energy = 1.0 + f0 * (1.0 / std::max(wrong_pair[1], 1.0e-5) - 1.0);
        const double wrong_k_s = std::max(0.0, (wrong_pair[0] * (1.0 - f0) + wrong_pair[1] * f0) * wrong_energy);
        const double wrong_channels = (1.0 - wrong_fresnel) * 0.5 + wrong_k_s * 0.5;
        for (u32 channel = 0u; channel < 3u; ++channel) {
            PPR_TEST_ASSERT(std::abs(ortho_pixel[channel] - literal_channels) <= 0.03);
        }
        PPR_TEST_ASSERT(std::abs(literal_channels - wrong_channels) > 0.1);
        std::println(
            "WORLD_LIGHTING_MATERIAL ortho_end_to_end pixel=({:.4f},{:.4f},{:.4f}) literal={:.4f} wrong={:.4f} "
            "NoV={:.6f} diffuse={:.4f}",
            ortho_pixel[0], ortho_pixel[1], ortho_pixel[2], literal_channels, wrong_channels, ortho_n_dot_v,
            (1.0 - fresnel) * 0.5);

        // --------------------------------------------------------------
        // perspective tilted camera: centre ray hits (0.5,0.5,0); literal
        // eye=origin must be distinguished from the old origin+forward eye.
        // --------------------------------------------------------------
        CameraModel perspective_model{};
        perspective_model.m_camera_mode = ECameraProjection::perspective;
        perspective_model.m_origin = float3{0.0f, 0.0f, 0.5f};
        perspective_model.m_basis = Quaternion::rotate(math::axis_z, normalize(float3{1.0f, 1.0f, -1.0f}));
        Camera perspective_camera{ECameraProjection::perspective};
        perspective_camera.updateModel(TimeSpan{}, perspective_model, Viewport{PixelRect{0, 0, 1, 1}});
        const auto perspective_snapshot = perspective_camera.getSnapshot();
        const double t = -static_cast<double>(perspective_snapshot.m_origin.z) /
                         static_cast<double>(perspective_snapshot.m_forward.z);
        const Double3 sample{
            static_cast<double>(perspective_snapshot.m_origin.x) +
                static_cast<double>(perspective_snapshot.m_forward.x) * t,
            static_cast<double>(perspective_snapshot.m_origin.y) +
                static_cast<double>(perspective_snapshot.m_forward.y) * t,
            0.0};
        const Double3 literal_view = normalized(
            {static_cast<double>(perspective_snapshot.m_origin.x) - sample.x,
                static_cast<double>(perspective_snapshot.m_origin.y) - sample.y,
                static_cast<double>(perspective_snapshot.m_origin.z) - sample.z},
            {0.0, 0.0, 1.0});
        const Double3 wrong_eye{
            static_cast<double>(perspective_snapshot.m_origin.x) +
                static_cast<double>(perspective_snapshot.m_forward.x),
            static_cast<double>(perspective_snapshot.m_origin.y) +
                static_cast<double>(perspective_snapshot.m_forward.y),
            static_cast<double>(perspective_snapshot.m_origin.z) +
                static_cast<double>(perspective_snapshot.m_forward.z)};
        const Double3 wrong_view = normalized(
            {wrong_eye.x - sample.x, wrong_eye.y - sample.y, wrong_eye.z - sample.z}, {0.0, 0.0, 1.0});
        const lighting::CH perspective_ch = directionalCH(float3{0.5f, 0.5f, 0.5f}, float3{0.5f, 0.5f, 0.5f});
        const auto perspective_pixel = render_pixel(perspective_snapshot, perspective_ch);

        const auto evaluate = [&](const Double3 view) {
            const double n_dot_v = std::max(view.z, 1.0e-5);
            const auto pair = sampleLutCpu(lut, n_dot_v, 0.5);
            const double fresnel_value = f0 +
                                         (std::max(1.0 - 0.5, f0) - f0) * std::pow(1.0 - std::min(n_dot_v, 1.0), 5.0);
            const double energy_value = 1.0 + f0 * (1.0 / std::max(pair[1], 1.0e-5) - 1.0);
            const double specular_weight = std::max(0.0, (pair[0] * (1.0 - f0) + pair[1] * f0) * energy_value);
            const Double3 reflection = normalized({-view.x, -view.y, -view.z + 2.0 * n_dot_v}, {0.0, 0.0, 1.0});
            const Double3 diffuse = diffuseCpu(perspective_ch, {0.0, 0.0, 1.0});
            const Double3 specular = specularCpu(perspective_ch, reflection, 0.5);
            return Double3{
                (1.0 - fresnel_value) / kPi * diffuse.x + specular_weight * specular.x,
                (1.0 - fresnel_value) / kPi * diffuse.y + specular_weight * specular.y,
                (1.0 - fresnel_value) / kPi * diffuse.z + specular_weight * specular.z};
        };
        const Double3 literal_response = evaluate(literal_view);
        const Double3 wrong_response = evaluate(wrong_view);
        const std::array<double, 3u> literal_values{literal_response.x, literal_response.y, literal_response.z};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            PPR_TEST_ASSERT(std::abs(perspective_pixel[channel] - literal_values[channel]) <= 0.03);
        }
        const double literal_luma = literal_response.x + literal_response.y + literal_response.z;
        const double wrong_luma = wrong_response.x + wrong_response.y + wrong_response.z;
        PPR_TEST_ASSERT(std::abs(literal_luma - wrong_luma) > 0.01);
        std::println(
            "WORLD_LIGHTING_MATERIAL perspective_end_to_end pixel=({:.4f},{:.4f},{:.4f}) literal=({:.4f},{:.4f},"
            "{:.4f}) wrong=({:.4f},{:.4f},{:.4f})",
            perspective_pixel[0], perspective_pixel[1], perspective_pixel[2], literal_response.x, literal_response.y,
            literal_response.z, wrong_response.x, wrong_response.y, wrong_response.z);
    };

    PPR_UNIT_TEST (grid_frame_reflection_matches_the_cpu_contract) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "grid_tiles.slang", "grid_tiles", module.writeRef()));
        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        slang::TypeLayoutReflection *frame = nullptr;
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_grid_frame") {
                frame = parameter->getTypeLayout()->getElementTypeLayout();
                break;
            }
        }

        PPR_TEST_ASSERT(frame != nullptr);
        std::println(
            "WORLD_LIGHTING_MATERIAL grid_frame size={} cpu={}", frame->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM),
            sizeof(GridPass::GridFrame));
        for (unsigned index = 0u; index < frame->getFieldCount(); ++index) {
            auto *const field = frame->getFieldByIndex(index);
            std::println(
                "WORLD_LIGHTING_MATERIAL grid_frame field[{}] name={} offset={}", index, field->getName(),
                field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM));
        }

        PPR_TEST_ASSERT(frame->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GridPass::GridFrame));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 8u>{
                 std::pair{"m_camera_world_x", 304u}, {"m_camera_world_y", 308u}, {"m_camera_world_z", 312u},
                 {"m_camera_forward_x", 316u}, {"m_camera_forward_y", 320u}, {"m_camera_forward_z", 324u},
                 {"m_camera_ortho", 328u}, {"m_has_resolved_cells", 332u}}) {
            bool matched = false;
            for (unsigned index = 0u; index < frame->getFieldCount(); ++index) {
                auto *const field = frame->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    PPR_TEST_ASSERT(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset);
                    matched = true;
                    break;
                }
            }

            PPR_TEST_ASSERT(matched);
        }
    };

    PPR_UNIT_TEST (grid_tiles_sample_rate_boundary_selects_distinct_cell_materials) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        Array<u32> resolved(static_cast<std::size_t>(WorldLightingPass::kCellCount), 0u);
        resolved[2u * WorldLightingPass::kWorldEdge + 0u] = 1u;
        resolved[2u * WorldLightingPass::kWorldEdge + 1u] = 2u;

        GridTerrainVisual red{};
        red.m_material_id = 1u;
        red.m_color_rgba = 0xffffffffu;
        red.m_emissive_rgb[0] = 1.0f;
        red.m_emissive_strength = 1.0f;
        red.m_ior = 1.5f;
        red.m_roughness = 0.5f;
        red.m_opacity = 1.0f;
        GridTerrainVisual green = red;
        green.m_material_id = 2u;
        green.m_emissive_rgb[0] = 0.0f;
        green.m_emissive_rgb[1] = 1.0f;
        const std::array<GridTerrainVisual, 2u> visuals{red, green};

        const auto samples = renderResolvedCells(fixture, renderer, grid, resolved, visuals);
        std::println(
            "WORLD_LIGHTING_MATERIAL sample_rate samples=({:.3f},{:.3f},{:.3f},{:.3f}) "
            "({:.3f},{:.3f},{:.3f},{:.3f}) "
            "({:.3f},{:.3f},{:.3f},{:.3f}) "
            "({:.3f},{:.3f},{:.3f},{:.3f})",
            samples.m_rgba[0][0], samples.m_rgba[0][1], samples.m_rgba[0][2], samples.m_rgba[0][3],
            samples.m_rgba[1][0], samples.m_rgba[1][1], samples.m_rgba[1][2], samples.m_rgba[1][3],
            samples.m_rgba[2][0], samples.m_rgba[2][1], samples.m_rgba[2][2], samples.m_rgba[2][3],
            samples.m_rgba[3][0], samples.m_rgba[3][1], samples.m_rgba[3][2], samples.m_rgba[3][3]);
        for (const u32 sample: {0u, 2u}) {
            PPR_TEST_ASSERT(samples.m_rgba[sample][0] > 0.5f and samples.m_rgba[sample][1] < 0.05f);
        }

        for (const u32 sample: {1u, 3u}) {
            PPR_TEST_ASSERT(samples.m_rgba[sample][0] < 0.05f and samples.m_rgba[sample][1] > 0.5f);
        }
    };

    PPR_UNIT_TEST (opaque_thermal_glow_and_receive_only_thermal_exclusion) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        GridTerrainVisual opaque{};
        opaque.m_material_id = 1u;
        opaque.m_color_rgba = 0xffffffffu;
        opaque.m_ior = 1.5f;
        opaque.m_roughness = 0.5f;
        opaque.m_opacity = 1.0f;
        GridTerrainVisual glass = opaque;
        glass.m_material_id = 2u;
        glass.m_surface_flags = kGridSurfaceOpticalBlend;
        glass.m_opacity = 0.5f;
        const std::array<GridTerrainVisual, 2u> visuals{opaque, glass};

        const auto cell = [](const float kelvin, const u32 material) {
            const auto packed = lighting::packCell(static_cast<u16>(material), static_cast<double>(kelvin));
            PPR_TEST_ASSERT(packed.has_value());
            return *packed;
        };

        Array<u32> cold(static_cast<std::size_t>(WorldLightingPass::kCellCount), 0u);
        Array<u32> hot = cold;
        Array<u32> glass_hot = cold;
        Array<u32> glass_cold = cold;
        for (const u32 x: {0u, 1u}) {
            cold[2u * WorldLightingPass::kWorldEdge + x] = cell(0.0, 1u);
            hot[2u * WorldLightingPass::kWorldEdge + x] = cell(4095.0, 1u);
            glass_hot[2u * WorldLightingPass::kWorldEdge + x] = cell(4095.0, 2u);
            glass_cold[2u * WorldLightingPass::kWorldEdge + x] = cell(0.0, 2u);
        }

        const auto opaque_cold = renderResolvedCells(fixture, renderer, grid, cold, visuals);
        const auto opaque_hot = renderResolvedCells(fixture, renderer, grid, hot, visuals);
        const auto transparent_hot = renderResolvedCells(fixture, renderer, grid, glass_hot, visuals);
        const auto transparent_cold = renderResolvedCells(fixture, renderer, grid, glass_cold, visuals);

        const float opaque_cold_luma = opaque_cold.m_rgba[0][0] + opaque_cold.m_rgba[0][1] + opaque_cold.m_rgba[0][2];
        const float opaque_hot_luma = opaque_hot.m_rgba[0][0] + opaque_hot.m_rgba[0][1] + opaque_hot.m_rgba[0][2];
        PPR_TEST_ASSERT(opaque_hot_luma > opaque_cold_luma + 0.05f);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            PPR_TEST_ASSERT(
                std::abs(transparent_hot.m_rgba[0][channel] - transparent_cold.m_rgba[0][channel]) <= 0.002f);
        }

        // Transparent receive-only cells use premultiplied authored opacity and
        // never add their thermal color. Compare with an otherwise identical
        // opaque response over the fixed clear color.
        const float3 ambient{0.6f, 0.5f, 0.4f};
        const auto opaque_ambient = renderResolvedCells(fixture, renderer, grid, cold, visuals, ambient);
        const auto transparent_ambient = renderResolvedCells(fixture, renderer, grid, glass_cold, visuals, ambient);
        const std::array<float, 3u> clear{0.1f, 0.1f, 0.2f};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            const float expected = 0.5f * opaque_ambient.m_rgba[0][channel] + 0.5f * clear[channel];
            PPR_TEST_ASSERT(std::abs(transparent_ambient.m_rgba[0][channel] - expected) <= 0.01f);
        }

        std::println(
            "WORLD_LIGHTING_MATERIAL thermal opaque_cold={:.4f} opaque_hot={:.4f} transparent_hot={:.4f} "
            "transparent_cold={:.4f}",
            opaque_cold_luma, opaque_hot_luma, transparent_hot.m_rgba[0][0], transparent_cold.m_rgba[0][0]);
    };

    PPR_UNIT_TEST (gas_angular_mean_uses_the_decoded_dc_field) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        constexpr u32 edge = lighting::kFineCHEdge;
        Array<lighting::PackedCH> records(static_cast<std::size_t>(edge) * edge);
        for (auto &record: records) {
            const auto encoded = lighting::packCH(uniformCH(float3{0.25f, 0.5f, 0.75f}));
            PPR_TEST_ASSERT(encoded.has_value());
            record = *encoded;
        }

        auto &device = fixture.device();
        auto fine = makeBuffer(
            device, records.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH), records.data());
        std::array<ProbeCase, 2u> cases{};
        cases[0].m_lookup = float4{1.0f, 1.0f, 0.0f, 0.0f};
        cases[1].m_lookup = float4{100.5f, 200.5f, 0.0f, 0.0f};
        const auto result = runProbe(fixture, "probeAngularMean", cases, nullptr, fine.get());
        std::println(
            "WORLD_LIGHTING_MATERIAL gas_mean actual=({:.6f},{:.6f},{:.6f})", float(result[0].x), float(result[0].y),
            float(result[0].z));
        PPR_TEST_ASSERT(close3(float3{result[0].x, result[0].y, result[0].z}, float3{0.25f, 0.5f, 0.75f}, 0.005f));
        PPR_TEST_ASSERT(close3(float3{result[1].x, result[1].y, result[1].z}, float3{0.25f, 0.5f, 0.75f}, 0.005f));
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // sample-rate categorical material, shared BRDF table and gas suite
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_materialTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_material") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingMaterial::brdf_lut_matches_independent_binary64_integration,
                detail::WorldLightingMaterial::brdf_lut_sampling_clamps_at_both_edges,
                detail::WorldLightingMaterial::black_environment_and_no_emission_is_black,
                detail::WorldLightingMaterial::perspective_and_orthographic_view_vectors_match_the_eye_contract,
                detail::WorldLightingMaterial::categorical_view_vector_matches_the_literal_grid_frame_path,
                detail::WorldLightingMaterial::categorical_base_color_normal_bombing_and_period_contracts,
                detail::WorldLightingMaterial::doubling_environment_doubles_reflected_hdr,
                detail::WorldLightingMaterial::metallic_and_roughness_endpoints_change_visible_response,
                detail::WorldLightingMaterial::authored_albedo_and_normal_change_visible_response,
                detail::WorldLightingMaterial::uniform_environment_is_independent_of_ch_direction,
                detail::WorldLightingMaterial::zero_xy_normal_and_reflection_are_continuous,
                detail::WorldLightingMaterial::authored_emission_is_added_once_and_thermal_matches_source,
                detail::WorldLightingMaterial::gas_angular_mean_uses_the_decoded_dc_field,
                detail::WorldLightingMaterial::gas_uses_nonzero_ch_angular_mean_and_alpha_composition,
                detail::WorldLightingMaterial::grid_tiles_sample_rate_boundary_selects_distinct_cell_materials,
                detail::WorldLightingMaterial::grid_frame_reflection_matches_the_cpu_contract,
                detail::WorldLightingMaterial::opaque_thermal_glow_and_receive_only_thermal_exclusion,
            });
        };
        return group;
    }
}
