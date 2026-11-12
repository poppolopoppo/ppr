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
        UnitTest::IRun &_, Fixture &fixture, Renderer &renderer, GridPass &grid, const std::span<const u32> resolved,
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
        _.log(std::format("WORLD_LIGHTING_MATERIAL planned_tiles={}", plan->m_tiles.size()));
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

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL brdf_lut={}x{} rg16f bytes={} max_binary64_error={:.6f}", kWidth, kHeight,
                bytes, maximum_error));
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
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL brdf_clamp low=({:.6f},{:.6f}) high=({:.6f},{:.6f})", float(sampled[0].x),
                float(sampled[0].y), float(sampled[2].x), float(sampled[2].y)));
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

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL view ortho=({:.4f},{:.4f},{:.4f}) centre=({:.4f},{:.4f},{:.4f}) "
                "offcentre=({:.4f},{:.4f},{:.4f}) noV={:.6f}",
                float(detail[0].x), float(detail[0].y), float(detail[0].z), float(detail[2].x), float(detail[2].y),
                float(detail[2].z), float(detail[4].x), float(detail[4].y), float(detail[4].z), float(detail[4].w)));
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
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL uniform_diffuse=({:.6f},{:.6f},{:.6f}) specular=({:.6f},{:.6f},{:.6f})",
                float(diffuse[0].x), float(diffuse[0].y), float(diffuse[0].z), float(specular[0].x),
                float(specular[0].y), float(specular[0].z)));
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
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL categorical detail5=({:.6f},{:.6f},{:.6f},{:.6f}) detail6=({:.6f},{:.6f},"
                "{:.6f},{:.6f})",
                float(field(0u, 5u).x), float(field(0u, 5u).y), float(field(0u, 5u).z), float(field(0u, 5u).w),
                float(field(0u, 6u).x), float(field(0u, 6u).y), float(field(0u, 6u).z), float(field(0u, 6u).w)));
        PPR_TEST_ASSERT(std::abs(float(field(0u, 5u).y) - expected_gradient) <= 1.0e-6f);
        PPR_TEST_ASSERT(std::abs(float(field(0u, 5u).z) - 0.02f) <= 1.0e-6f);
        PPR_TEST_ASSERT(std::abs(float(field(6u, 5u).y) - 0.01f / 2.0f) <= 1.0e-6f);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL categorical base=({:.4f},{:.4f},{:.4f}) normal_x={:.4f} normal_y={:.4f} "
                "uv_gradient={:.6f} period2_gradient={:.6f}",
                float(base0.x), float(base0.y), float(base0.z), float(normal_x_result.z), float(normal_y_result.w),
                float(field(0u, 5u).y), float(field(6u, 5u).y)));
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
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL gas lit=({:.4f},{:.4f},{:.4f},{:.4f}) dark=({:.4f},{:.4f},{:.4f},{:.4f}) "
                "alpha={:.4f} instances={} draws={}",
                lit[0], lit[1], lit[2], lit[3], dark[0], dark[1], dark[2], dark[3], alpha,
                grid.observations().m_particle_instances, grid.observations().m_draw_commands));
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
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL ortho_end_to_end pixel=({:.4f},{:.4f},{:.4f}) literal={:.4f} wrong={:.4f} "
                "NoV={:.6f} diffuse={:.4f}",
                ortho_pixel[0], ortho_pixel[1], ortho_pixel[2], literal_channels, wrong_channels, ortho_n_dot_v,
                (1.0 - fresnel) * 0.5));

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
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL perspective_end_to_end pixel=({:.4f},{:.4f},{:.4f}) literal=({:.4f},{:.4f},"
                "{:.4f}) wrong=({:.4f},{:.4f},{:.4f})",
                perspective_pixel[0], perspective_pixel[1], perspective_pixel[2], literal_response.x,
                literal_response.y, literal_response.z, wrong_response.x, wrong_response.y, wrong_response.z));
    };

    PPR_UNIT_TEST (grid_frame_reflection_matches_the_cpu_contract) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        PPR_TEST_ASSERT(sizeof(GridPass::GridFrame) == 336u);
        const auto verify_frame = [&](const char *const file, const char *const module_name) {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / file, module_name, module.writeRef()));
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
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL {} grid_frame size={} cpu={}", module_name,
                    frame->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM), sizeof(GridPass::GridFrame)));
            for (unsigned index = 0u; index < frame->getFieldCount(); ++index) {
                auto *const field = frame->getFieldByIndex(index);
                _.log(
                    std::format(
                        "WORLD_LIGHTING_MATERIAL {} grid_frame field[{}] name={} offset={}", module_name, index,
                        field->getName(), field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM)));
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

        verify_frame("grid_tiles.slang", "grid_tiles");
        verify_frame("grid_background.slang", "grid_background");
    };

    PPR_UNIT_TEST (background_layers_receive_light_at_bounded_offsets) {
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

        // One opaque white layer carries every assertion; the three upper
        // layers are authored fully transparent so ordering cannot leak in.
        GridTerrainVisual visual{};
        visual.m_material_id = 1u;
        visual.m_color_rgba = 0xffffffffu;
        visual.m_ior = 1.5f;
        visual.m_roughness = 0.5f;
        visual.m_opacity = 1.0f;
        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_has_background = true;
        stage.m_terrain_visuals = {visual};
        stage.m_terrain_images.push_back(makeConstantImage(float4{1.0f, 1.0f, 1.0f, 1.0f}));
        for (u32 layer = 0u; layer < 4u; ++layer) {
            stage.m_background_layers[layer] = GridBackgroundLayer{
                .m_texture_index = 0u,
                .m_order = layer,
                .m_color_rgba = layer == 0u ? 0xffffffffu : 0xffffff00u,
                .m_repeat_period_m = 8.0f,
                .m_parallax_factor = 0.0f,
            };
        }
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

        Array<u32> resolved(static_cast<std::size_t>(WorldLightingPass::kCellCount), 1u);
        auto resolved_buffer = makeBuffer(
            fixture.device(), resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        const auto make_ch = [&](const lighting::CH &ch) {
            Array<lighting::PackedCH> records(lighting::kFineCHRecordCount);
            const auto packed = lighting::packCH(ch);
            PPR_TEST_ASSERT(packed.has_value());
            for (auto &record: records) {
                record = *packed;
            }

            return makeBuffer(
                fixture.device(), records.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH),
                records.data());
        };

        // Eight metres across at 64 pixels. The orthographic origin is the
        // view's lower-left corner, so this frame covers [1016, 1024) metres:
        // the right column's unshifted receiver is inside the field and eight
        // metres from the camera centre.
        constexpr u32 kEdge = 64u;
        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{1016.0f, 1016.0f, -0.5f};
        model.m_ortho_scale = 0.125f;
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, kEdge, kEdge}});
        const auto snapshot = camera.getSnapshot();
        const auto viewport = rhi::Viewport::fromSize(float(kEdge), float(kEdge));
        const auto scissor = rhi::ScissorRect::fromSize(kEdge, kEdge);

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = kEdge, .height = kEdge, .depth = 1u};
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

        const auto capture = [&](const lighting::CH &ch) {
            PPR_TEST_ASSERT(not grid.setLightingBuffersForTest(resolved_buffer, make_ch(ch)));
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot, false));
            const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                if (const auto ec = grid.prepareBackgroundInputs(context, snapshot, viewport, scissor)) {
                    return ec;
                }

                return grid.prepareBackground(context, snapshot, viewport, scissor);
            };
            const auto draw = [&](const DrawContext &context) -> std::error_code {
                return grid.renderBackground(context);
            };
            DrawSubmission submission{"background receiver layers", DrawCallback{draw}};
            submission.m_viewport = viewport;
            submission.m_scissor = scissor;
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {submission}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            return readTexture(fixture.device(), *target);
        };
        const auto pixel = [](const TextureReadback &readback, const u32 x, const u32 y) {
            const auto *const words = reinterpret_cast<const u16 *>(
                readback.m_bytes.data() + static_cast<std::size_t>(y) * readback.m_row_pitch + x * 8u);
            std::array<float, 4u> value{};
            for (u32 channel = 0u; channel < 4u; ++channel) {
                value[channel] = halfToFloat(words[channel]);
            }

            return value;
        };

        const auto assert_black = [&](const std::array<float, 4u> &value) {
            PPR_TEST_ASSERT(std::abs(value[0]) <= 0.003f);
            PPR_TEST_ASSERT(std::abs(value[1]) <= 0.003f);
            PPR_TEST_ASSERT(std::abs(value[2]) <= 0.003f);
        };

        // A nonunit uniform field must appear exactly once: squaring it or
        // applying it to alpha fails these values.
        const auto lit = capture(uniformCH(float3{0.5f, 0.25f, 0.125f}));
        const auto lit_center = pixel(lit, kEdge / 2u, kEdge / 2u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL background_receiver lit=({:.4f},{:.4f},{:.4f},{:.4f})", lit_center[0],
                lit_center[1], lit_center[2], lit_center[3]));
        PPR_TEST_ASSERT(std::abs(lit_center[0] - 0.5f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(lit_center[1] - 0.25f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(lit_center[2] - 0.125f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(lit_center[3] - 1.0f) <= 0.003f);

        // A zero field yields zero reflected HDR while the layer stays opaque.
        const auto dark = capture(lighting::CH{});
        const auto dark_center = pixel(dark, kEdge / 2u, kEdge / 2u);
        assert_black(dark_center);
        PPR_TEST_ASSERT(std::abs(dark_center[3] - 1.0f) <= 0.003f);

        // The right column's unshifted point is inside the map, but the bounded
        // depth offset pushes its CH position outside; that layer is zero.
        const auto exterior = pixel(lit, kEdge - 1u, kEdge / 2u);
        assert_black(exterior);
        PPR_TEST_ASSERT(std::abs(exterior[3] - 1.0f) <= 0.003f);
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

        const auto samples = renderResolvedCells(_, fixture, renderer, grid, resolved, visuals);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL sample_rate samples=({:.3f},{:.3f},{:.3f},{:.3f}) "
                "({:.3f},{:.3f},{:.3f},{:.3f}) "
                "({:.3f},{:.3f},{:.3f},{:.3f}) "
                "({:.3f},{:.3f},{:.3f},{:.3f})",
                samples.m_rgba[0][0], samples.m_rgba[0][1], samples.m_rgba[0][2], samples.m_rgba[0][3],
                samples.m_rgba[1][0], samples.m_rgba[1][1], samples.m_rgba[1][2], samples.m_rgba[1][3],
                samples.m_rgba[2][0], samples.m_rgba[2][1], samples.m_rgba[2][2], samples.m_rgba[2][3],
                samples.m_rgba[3][0], samples.m_rgba[3][1], samples.m_rgba[3][2], samples.m_rgba[3][3]));
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

        const auto opaque_cold = renderResolvedCells(_, fixture, renderer, grid, cold, visuals);
        const auto opaque_hot = renderResolvedCells(_, fixture, renderer, grid, hot, visuals);
        const auto transparent_hot = renderResolvedCells(_, fixture, renderer, grid, glass_hot, visuals);
        const auto transparent_cold = renderResolvedCells(_, fixture, renderer, grid, glass_cold, visuals);

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
        const auto opaque_ambient = renderResolvedCells(_, fixture, renderer, grid, cold, visuals, ambient);
        const auto transparent_ambient = renderResolvedCells(_, fixture, renderer, grid, glass_cold, visuals, ambient);
        const std::array<float, 3u> clear{0.1f, 0.1f, 0.2f};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            const float expected = 0.5f * opaque_ambient.m_rgba[0][channel] + 0.5f * clear[channel];
            PPR_TEST_ASSERT(std::abs(transparent_ambient.m_rgba[0][channel] - expected) <= 0.01f);
        }

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL thermal opaque_cold={:.4f} opaque_hot={:.4f} transparent_hot={:.4f} "
                "transparent_cold={:.4f}",
                opaque_cold_luma, opaque_hot_luma, transparent_hot.m_rgba[0][0], transparent_cold.m_rgba[0][0]));
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
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL gas_mean actual=({:.6f},{:.6f},{:.6f})", float(result[0].x),
                float(result[0].y), float(result[0].z)));
        PPR_TEST_ASSERT(close3(float3{result[0].x, result[0].y, result[0].z}, float3{0.25f, 0.5f, 0.75f}, 0.005f));
        PPR_TEST_ASSERT(close3(float3{result[1].x, result[1].y, result[1].z}, float3{0.25f, 0.5f, 0.75f}, 0.005f));
    };

    // ------------------------------------------------------------------
    // retained inspection overlay
    // ------------------------------------------------------------------
    // The real world_lighting_inspect.slang is rendered standalone against
    // controlled published records. The assertions decode the same records on
    // the CPU, so a view that silently changes its mapping fails here.

    inline constexpr u32 kInspectTarget = 512u;
    inline constexpr u32 kInspectSmall = 64u;

    struct InspectionFrame {
        float4x4 m_inverse_view_projection{identity_v};
        float4 m_viewport{};
        u32 m_diagnostic{};
        u32 m_cascade_level{1u};
        u32 m_encode_srgb{};
        float m_cells_per_metre{1.0f};
    };
    static_assert(sizeof(InspectionFrame) == 96u);
    static_assert(PPR_OFFSETOF(InspectionFrame, m_cells_per_metre) == 92u);

    [[nodiscard]] CameraSnapshot inspectionCamera() noexcept {
        CameraSnapshot camera{};
        const float world_edge = static_cast<float>(WorldLightingPass::kWorldEdge);
        camera.m_origin = float3{0.0f, 0.0f, -1.0f};
        camera.m_view = float4x4::translate(0.0f, 0.0f, 1.0f);
        camera.m_projection = rhi::getOrthoMatrix(world_edge, world_edge);
        camera.m_view_projection = camera.m_view * camera.m_projection;
        camera.m_invert_view_projection = inverse(camera.m_view_projection);
        camera.m_viewport_size = float2{world_edge, world_edge};
        camera.m_aspect_ratio = 1.0f;
        return camera;
    }

    // A translated/zoomed camera over a native-cell span at the gameplay cell
    // metric. The shader unprojects to metres and converts once, so the CPU
    // mirror here must carry the same cells-per-metre scalar.
    [[nodiscard]] CameraSnapshot inspectionMetricCamera(
        const float cells_per_metre, const float2 min_native, const float2 span_native) noexcept {
        CameraSnapshot camera{};
        const float3 origin{min_native.x / cells_per_metre, min_native.y / cells_per_metre, -1.0f};
        camera.m_origin = origin;
        camera.m_view = float4x4::translate(-origin.x, -origin.y, 1.0f);
        camera.m_projection = rhi::getOrthoMatrix(span_native.x / cells_per_metre, span_native.y / cells_per_metre);
        camera.m_view_projection = camera.m_view * camera.m_projection;
        camera.m_invert_view_projection = inverse(camera.m_view_projection);
        camera.m_viewport_size = span_native;
        camera.m_aspect_ratio = span_native.x / span_native.y;
        return camera;
    }

    // CPU mirror of the shader's worldOnPlane: pixel -> native cell coordinate.
    [[nodiscard]] float2 inspectionNativeAt(
        const CameraSnapshot &camera, const float2 pixel, const float cells_per_metre, const int2 extent) noexcept {
        const float2 uv{pixel.x / static_cast<float>(extent.x), pixel.y / static_cast<float>(extent.y)};
        const float2 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};

        float4 near_h = float4{ndc.x, ndc.y, 0.0f, 1.0f} * camera.m_invert_view_projection;
        float4 far_h = float4{ndc.x, ndc.y, 1.0f, 1.0f} * camera.m_invert_view_projection;
        near_h = near_h / near_h[3];
        far_h = far_h / far_h[3];

        const float3 direction{far_h.x - near_h.x, far_h.y - near_h.y, far_h.z - near_h.z};
        PPR_TEST_ASSERT(std::abs(direction.z) > 1.0e-12f);
        const float3 world{
            near_h.x + direction.x * (-near_h.z / direction.z), near_h.y + direction.y * (-near_h.z / direction.z),
            0.0f};
        return float2{world.x * cells_per_metre, world.y * cells_per_metre};
    }

    // CPU mirror of the same mapping: native cell coordinate -> pixel.
    [[nodiscard]] float2 inspectionPixelAt(
        const CameraSnapshot &camera, const float2 native, const float cells_per_metre, const int2 extent) noexcept {
        const float4 clip = float4{native.x / cells_per_metre, native.y / cells_per_metre, 0.0f, 1.0f} *
                            camera.m_view_projection;
        const float2 ndc{clip.x / clip[3], clip.y / clip[3]};
        return float2{
            (ndc.x + 1.0f) * 0.5f * static_cast<float>(extent.x), (1.0f - ndc.y) * 0.5f * static_cast<float>(extent.y)};
    }

    [[nodiscard]] float inspectionCurveScalar(const float3 rgb) noexcept {
        const float peak = (std::max) (rgb.x, (std::max) (rgb.y, rgb.z));
        return 1.0f / (1.0f + (std::max) (peak, 0.0f));
    }

    [[nodiscard]] float inspectionSrgbEncode(const float linear) noexcept {
        const float bounded = (std::max) (linear, 0.0f);
        return bounded <= 0.0031308f ? bounded * 12.92f : 1.055f * std::pow(bounded, 1.0f / 2.4f) - 0.055f;
    }

    [[nodiscard]] std::array<float, 3u> inspectionCurve(const float3 rgb) noexcept {
        const float3 positive{(std::max) (rgb.x, 0.0f), (std::max) (rgb.y, 0.0f), (std::max) (rgb.z, 0.0f)};
        const float scale = inspectionCurveScalar(positive);
        return {positive.x * scale, positive.y * scale, positive.z * scale};
    }

    [[nodiscard]] rhi::ComPtr<rhi::ITexture> makeInspectionTarget(rhi::IDevice &device, const int2 extent) {
        rhi::TextureDesc description{};
        description.type = rhi::TextureType::Texture2D;
        description.size = {static_cast<u32>(extent.x), static_cast<u32>(extent.y), 1u};
        description.arrayLength = 1u;
        description.mipCount = 1u;
        description.format = rhi::Format::RGBA8Unorm;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
        description.defaultState = rhi::ResourceState::RenderTarget;
        description.label = "world lighting inspection target";
        rhi::ComPtr<rhi::ITexture> target{};
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(description, nullptr, target.writeRef())));
        return target;
    }

    struct InspectionResources {
        rhi::ComPtr<rhi::IBuffer> m_resolved{};
        rhi::ComPtr<rhi::IBuffer> m_visuals{};
        rhi::ComPtr<rhi::IBuffer> m_fine_ch{};
        rhi::ComPtr<rhi::IBuffer> m_source{};
        rhi::ComPtr<rhi::IBuffer> m_cascades{};
        rhi::ComPtr<rhi::ITexture> m_jump{};
    };

    struct InspectionRenderer {
        Fixture &m_fixture;
        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IRenderPipeline> m_pipeline{};
        rhi::ComPtr<rhi::IShaderObject> m_root{};

        explicit InspectionRenderer(Fixture &fixture) : m_fixture(fixture) {
            auto &device = fixture.device();
            shader::SharedModule module{};
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / "world_lighting_inspect.slang", "world_lighting_inspect",
                module.writeRef()));

            constexpr const char *entries[]{"vertexInspect", "fragmentInspect"};
            std::array<shader::ComPtr<slang::IEntryPoint>, 2u> points{};
            for (u32 index = 0u; index < points.size(); ++index) {
                PPR_TEST_ASSERT(
                    not make_error_code(module->findEntryPointByName(entries[index], points[index].writeRef())));
            }

            slang::IComponentType *entry_points[]{points[0u].get(), points[1u].get()};
            rhi::ShaderProgramDesc program_description{};
            program_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_description.slangGlobalScope = module.get();
            program_description.slangEntryPoints = entry_points;
            program_description.slangEntryPointCount = 2u;
            shader::Diagnose diagnostics;
            PPR_TEST_ASSERT(not make_error_code(
                device.createShaderProgram(program_description, m_program.writeRef(), diagnostics.writeRef())));

            rhi::ColorTargetDesc color{};
            color.format = rhi::Format::RGBA8Unorm;
            rhi::RenderPipelineDesc pipeline_description{};
            pipeline_description.program = m_program;
            pipeline_description.targets = &color;
            pipeline_description.targetCount = 1u;
            pipeline_description.multisample.sampleCount = 1u;
            pipeline_description.rasterizer.cullMode = rhi::CullMode::None;
            pipeline_description.depthStencil.depthTestEnable = false;
            pipeline_description.depthStencil.depthWriteEnable = false;
            PPR_TEST_ASSERT(
                not make_error_code(device.createRenderPipeline(pipeline_description, m_pipeline.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(m_program, m_root.writeRef())));
        }

        [[nodiscard]] TextureReadback draw(
            const InspectionResources &resources, const CameraSnapshot &camera, const u32 diagnostic, const u32 cascade,
            const int2 extent, const float cells_per_metre) {
            auto &device = m_fixture.device();
            auto target = makeInspectionTarget(device, extent);
            auto target_view = defaultView(target);

            InspectionFrame frame{};
            frame.m_inverse_view_projection = camera.m_invert_view_projection;
            frame.m_viewport = float4{
                static_cast<float>(extent.x), static_cast<float>(extent.y), 1.0f / static_cast<float>(extent.x),
                1.0f / static_cast<float>(extent.y)};
            frame.m_diagnostic = diagnostic;
            frame.m_cascade_level = cascade;
            // The test target is UNorm: the shader owns the one sRGB encode,
            // exactly like an SDR surface.
            frame.m_encode_srgb = 1u;
            frame.m_cells_per_metre = cells_per_metre;

            rhi::ShaderCursor cursor{m_root};
            rhi::ShaderCursor constants;
            PPR_TEST_ASSERT(not make_error_code(cursor["g_inspect"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));

            const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) {
                PPR_TEST_ASSERT(not make_error_code(
                    cursor[name].setBinding(rhi::Binding(buffer, rhi::BufferRange{0u, buffer->getDesc().size}))));
            };
            bind("g_resolved_cells", resources.m_resolved);
            bind("g_visuals", resources.m_visuals);
            bind("g_fine_ch", resources.m_fine_ch);
            bind("g_cell_source", resources.m_source);
            bind("g_cascades", resources.m_cascades);
            PPR_TEST_ASSERT(not make_error_code(cursor["g_jump"].setBinding(rhi::Binding(resources.m_jump))));

            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            PPR_TEST_ASSERT(not make_error_code(m_fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
            encoder->setTextureState(target.get(), range, rhi::ResourceState::RenderTarget);
            const std::array<rhi::RenderPassColorAttachment, 1u> colors{
                rhi::RenderPassColorAttachment{.view = target_view, .clearValue = {0.0f, 0.0f, 0.0f, 1.0f}}};
            rhi::RenderPassDesc pass_description{};
            pass_description.colorAttachments = colors.data();
            pass_description.colorAttachmentCount = 1u;
            auto *const pass = encoder->beginRenderPass(pass_description);
            PPR_TEST_ASSERT(pass != nullptr);
            pass->setRenderState(
                {.viewports = {rhi::Viewport::fromSize(static_cast<float>(extent.x), static_cast<float>(extent.y))},
                    .viewportCount = 1u,
                    .scissorRects = {rhi::ScissorRect::fromSize(
                        static_cast<u32>(extent.x), static_cast<u32>(extent.y))},
                    .scissorRectCount = 1u,
                    .vertexBuffers = {},
                    .indexBuffer = {}});
            pass->bindPipeline(m_pipeline, m_root);
            pass->draw({.vertexCount = 3u});
            pass->end();
            encoder->setTextureState(target.get(), range, rhi::ResourceState::CopySource);

            m_fixture.submit(*encoder);

            const auto readback = readTexture(device, *target);
            PPR_TEST_ASSERT(readback.m_bytes.size() >= static_cast<std::size_t>(extent.y) * readback.m_row_pitch);
            return readback;
        }

        [[nodiscard]] static std::array<u8, 4u> texel(const TextureReadback &pixels, const u32 x, const u32 y) {
            const auto *const bytes = pixels.m_bytes.data() + static_cast<std::size_t>(y) * pixels.m_row_pitch + x * 4u;
            return {
                static_cast<u8>(bytes[0]), static_cast<u8>(bytes[1]), static_cast<u8>(bytes[2]),
                static_cast<u8>(bytes[3])};
        }

        static void assertCategorical(
            UnitTest::IRun &_, const std::array<u8, 4u> &actual, const float3 color, const int tolerance = 2) {
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const int encoded = static_cast<int>(std::lround(
                    inspectionSrgbEncode(channel == 0u ? color.x : (channel == 1u ? color.y : color.z)) * 255.0f));
                if (std::abs(static_cast<int>(actual[channel]) - encoded) > tolerance) {
                    _.log(
                        std::format(
                            "INSPECT categorical mismatch channel={} actual={} expected={}", channel,
                            static_cast<int>(actual[channel]), encoded));
                }
                PPR_TEST_ASSERT(std::abs(static_cast<int>(actual[channel]) - encoded) <= tolerance);
            }
        }

        static void assertDisplayed(
            UnitTest::IRun &_, const std::array<u8, 4u> &actual, const float3 linear, const int tolerance = 2) {
            const auto expected = inspectionCurve(linear);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const int encoded = static_cast<int>(
                    std::lround(inspectionSrgbEncode(static_cast<float>(expected[channel])) * 255.0f));
                if (std::abs(static_cast<int>(actual[channel]) - encoded) > tolerance) {
                    _.log(
                        std::format(
                            "INSPECT mismatch channel={} actual={} expected={} linear=({},{},{})", channel,
                            static_cast<int>(actual[channel]), encoded, float(linear.x), float(linear.y),
                            float(linear.z)));
                }
                PPR_TEST_ASSERT(std::abs(static_cast<int>(actual[channel]) - encoded) <= tolerance);
            }
        }
    };

    [[nodiscard]] std::vector<std::byte> readBufferBytes(
        rhi::IDevice &device, rhi::IBuffer &buffer, const u64 offset, const u64 size) {
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        PPR_TEST_ASSERT(not make_error_code(device.readBuffer(&buffer, offset, size, bytes.data())));
        return bytes;
    }

    PPR_UNIT_TEST (inspection_settings_reject_unknown_views_and_levels) {
        for (const auto diagnostic:
            {ELightingDiagnostic::off, ELightingDiagnostic::probes, ELightingDiagnostic::source,
                ELightingDiagnostic::occupancy, ELightingDiagnostic::jump, ELightingDiagnostic::cascade}) {
            for (const u32 cascade: {1u, 5u}) {
                PPR_TEST_ASSERT(not validateLightingInspectionSettings({diagnostic, cascade}));
            }
        }

        PPR_TEST_ASSERT(
            validateLightingInspectionSettings({static_cast<ELightingDiagnostic>(42), 1u}) ==
            std::errc::invalid_argument);
        for (const u32 cascade: {0u, 6u}) {
            PPR_TEST_ASSERT(
                validateLightingInspectionSettings({ELightingDiagnostic::cascade, cascade}) ==
                std::errc::invalid_argument);
        }
    };

    PPR_UNIT_TEST (inspection_views_decode_published_records_without_mutation) {
        Fixture fixture;
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };
        auto &device = fixture.device();
        InspectionRenderer renderer{fixture};

        constexpr u32 edge = WorldLightingPass::kWorldEdge;
        constexpr u32 fine_edge = WorldLightingPass::kFineCHEdge;
        constexpr u32 cascade_level_records = lighting::kCascadeLevelRecords;

        InspectionResources inputs{};

        std::vector<u32> resolved(static_cast<std::size_t>(edge) * edge, 0u);
        std::vector<u32> source(static_cast<std::size_t>(edge) * edge, 0u);
        std::vector<u8> jump(static_cast<std::size_t>(edge) * edge, 0u);
        std::array<GridTerrainVisual, 6u> visuals{};
        for (u32 index = 0u; index < visuals.size(); ++index) {
            visuals[index].m_material_id = index;
            visuals[index].m_color_rgba = 0xffff'ffffu;
        }
        visuals[2u].m_surface_flags = kGridSurfaceOpticalBlend;
        visuals[2u].m_opacity = 0.25f;
        visuals[3u].m_emissive_rgb[0] = 1.0f;
        visuals[3u].m_emissive_strength = 4.0f;

        const auto cell = [](const u32 x, const u32 y) { return static_cast<std::size_t>(y) * edge + x; };
        resolved[cell(8u, 8u)] = *lighting::packCell(1u, 300.0);
        resolved[cell(24u, 8u)] = *lighting::packCell(2u, 300.0);
        resolved[cell(56u, 8u)] = *lighting::packCell(3u, 300.0);
        // Landmark row for the gameplay metric camera (h = 1/3 m): opaque white
        // at cell (135,592), and its same-row map border cell (0,592).
        resolved[cell(135u, 592u)] = *lighting::packCell(1u, 300.0);
        resolved[cell(0u, 592u)] = *lighting::packCell(1u, 300.0);
        const u32 source_word = *lighting::packRGB9E5(float3{0.25f, 0.5f, 1.0f});
        source[cell(8u, 8u)] = source_word;

        const std::array<u8, 6u> jump_values{1u, 2u, 4u, 8u, 16u, 32u};
        const std::array<u32, 6u> jump_cells{8u, 24u, 40u, 56u, 72u, 88u};
        for (u32 index = 0u; index < jump_cells.size(); ++index) {
            jump[cell(jump_cells[index], 8u)] = jump_values[index];
        }

        const auto uniform = lighting::packCH(uniformCH(float3{0.25f, 0.5f, 1.0f}));
        PPR_TEST_ASSERT(uniform.has_value());
        std::vector<lighting::PackedCH> fine(static_cast<std::size_t>(fine_edge) * fine_edge, *uniform);

        std::vector<lighting::PackedInterval> cascades(
            static_cast<std::size_t>(WorldLightingPass::kCascadeLevels) * cascade_level_records);
        const auto setInterval = [&](const u32 level, const float3 radiance, const float transmittance) {
            const auto packed = lighting::packInterval(
                lighting::Interval{.m_radiance = radiance, .m_transmittance = transmittance});
            PPR_TEST_ASSERT(packed.has_value());
            // Fill the small atlas neighborhood the sampled panel pixels hit
            // (fragment centers add half a pixel to the exact coordinate).
            for (u32 y = 8u; y <= 12u; ++y) {
                for (u32 x = 136u; x <= 140u; ++x) {
                    cascades[static_cast<std::size_t>(level - 1u) * cascade_level_records + y * edge + x] = *packed;
                }
            }
        };
        setInterval(1u, float3{0.25f, 0.5f, 1.0f}, 0.5f);
        setInterval(5u, float3{1.0f, 0.25f, 0.5f}, 0.25f);

        inputs.m_resolved = makeBuffer(device, resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        inputs.m_visuals = makeBuffer(device, sizeof(visuals), sizeof(GridTerrainVisual), visuals.data());
        inputs.m_fine_ch = makeBuffer(
            device, fine.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH), fine.data());
        inputs.m_source = makeBuffer(device, source.size() * sizeof(u32), sizeof(u32), source.data());
        inputs.m_cascades = makeBuffer(
            device, cascades.size() * sizeof(lighting::PackedInterval), sizeof(lighting::PackedInterval),
            cascades.data());

        rhi::TextureDesc jump_description{};
        jump_description.type = rhi::TextureType::Texture2D;
        jump_description.size = {edge, edge, 1u};
        jump_description.arrayLength = 1u;
        jump_description.mipCount = 1u;
        jump_description.format = rhi::Format::R8Uint;
        jump_description.memoryType = rhi::MemoryType::DeviceLocal;
        jump_description.usage = rhi::TextureUsage::ShaderResource;
        jump_description.defaultState = rhi::ResourceState::ShaderResource;
        const rhi::SubresourceData jump_initial{jump.data(), edge, static_cast<u64>(edge) * edge};
        PPR_TEST_ASSERT(
            not make_error_code(device.createTexture(jump_description, &jump_initial, inputs.m_jump.writeRef())));

        // Snapshot the inspected records before any overlay draw.
        const auto resolved_before = readBufferBytes(
            device, *inputs.m_resolved, cell(8u, 8u) * sizeof(u32), sizeof(u32));
        const auto source_before = readBufferBytes(device, *inputs.m_source, cell(8u, 8u) * sizeof(u32), sizeof(u32));
        const auto fine_before = readBufferBytes(
            device, *inputs.m_fine_ch, static_cast<u64>(7u * 64u + 9u) * sizeof(lighting::PackedCH),
            sizeof(lighting::PackedCH));
        const auto cascade1_before = readBufferBytes(
            device, *inputs.m_cascades, static_cast<u64>(8u * edge + 136u) * sizeof(lighting::PackedInterval),
            sizeof(lighting::PackedInterval));
        const auto cascade5_before = readBufferBytes(
            device, *inputs.m_cascades,
            (static_cast<u64>(4u) * cascade_level_records + static_cast<u64>(8u * edge + 136u)) *
                sizeof(lighting::PackedInterval),
            sizeof(lighting::PackedInterval));

        const CameraSnapshot camera = inspectionCamera();
        const auto probe_pixels = renderer.draw(
            inputs, camera, static_cast<u32>(ELightingDiagnostic::probes), 1u, int2{2048, 2048}, 1.0f);
        const auto source_pixels = renderer.draw(
            inputs, camera, static_cast<u32>(ELightingDiagnostic::source), 1u, int2{64, 64}, 1.0f);
        const auto occupancy_pixels = renderer.draw(
            inputs, camera, static_cast<u32>(ELightingDiagnostic::occupancy), 1u, int2{64, 64}, 1.0f);
        const auto jump_pixels = renderer.draw(
            inputs, camera, static_cast<u32>(ELightingDiagnostic::jump), 1u, int2{64, 64}, 1.0f);
        const auto cascade1_pixels = renderer.draw(
            inputs, camera, static_cast<u32>(ELightingDiagnostic::cascade), 1u, int2{512, 512}, 1.0f);
        const auto cascade5_pixels = renderer.draw(
            inputs, camera, static_cast<u32>(ELightingDiagnostic::cascade), 5u, int2{512, 512}, 1.0f);

        // probes: uniform fine field. A 2048-pixel target keeps the two-cell
        // fine-probe lattice a full pixel-width from the sample at cell centers
        // (edge = target/2048), so this isolates the CH angular mean.
        const float3 mean = lighting::unpackRGB9E5(uniform->m_words[0]) * static_cast<float>(kInvSqrtTwoPi);
        InspectionRenderer::assertDisplayed(_, InspectionRenderer::texel(probe_pixels, 8u, 2040u), mean);

        // source: decoded cached outgoing RGB at native cell (8,8).
        InspectionRenderer::assertDisplayed(
            _, InspectionRenderer::texel(source_pixels, 0u, 63u), lighting::unpackRGB9E5(source_word));

        // occupancy: opaque white, receive-only cyan, empty black, emissive opaque white.
        const auto occupancy = [&](const u32 x) { return InspectionRenderer::texel(occupancy_pixels, x, 63u); };
        const auto nearByte = [](const u8 actual, const u8 expected, const int tolerance = 1) {
            return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= tolerance;
        };
        const u8 white = static_cast<u8>(std::lround(inspectionSrgbEncode(1.0f) * 255.0f));
        const u8 cyan = static_cast<u8>(std::lround(inspectionSrgbEncode(1.0f) * 255.0f));
        PPR_TEST_ASSERT(
            nearByte(occupancy(0u)[0], white) and
            nearByte(occupancy(0u)[1], white) and
            nearByte(occupancy(0u)[2], white));
        PPR_TEST_ASSERT(
            nearByte(occupancy(1u)[0], 0u) and nearByte(occupancy(1u)[1], cyan) and nearByte(occupancy(1u)[2], cyan));
        PPR_TEST_ASSERT(
            nearByte(occupancy(2u)[0], 0u) and nearByte(occupancy(2u)[1], 0u) and nearByte(occupancy(2u)[2], 0u));
        PPR_TEST_ASSERT(
            nearByte(occupancy(3u)[0], white) and
            nearByte(occupancy(3u)[1], white) and
            nearByte(occupancy(3u)[2], white));

        // jump: every stored class is a distinct labeled color.
        const std::array<float3, 6u> jump_colors{
            float3{0.80f, 0.08f, 0.08f}, float3{0.95f, 0.45f, 0.05f}, float3{0.92f, 0.92f, 0.08f},
            float3{0.10f, 0.80f, 0.20f}, float3{0.10f, 0.35f, 0.95f}, float3{0.85f, 0.15f, 0.85f}};
        for (u32 index = 0u; index < jump_colors.size(); ++index) {
            InspectionRenderer::assertCategorical(
                _, InspectionRenderer::texel(jump_pixels, index, 63u), jump_colors[index]);
        }

        // cascade: left panel is radiance through the inspection curve, right
        // panel is transmittance. Pixel (34,130) maps to atlas (136,8) without
        // landing on the probe/direction lattice at either level.
        const auto left = [](const TextureReadback &pixels) { return InspectionRenderer::texel(pixels, 34u, 130u); };
        const auto right = [](const TextureReadback &pixels) { return InspectionRenderer::texel(pixels, 290u, 130u); };
        InspectionRenderer::assertDisplayed(_, left(cascade1_pixels), float3{0.25f, 0.5f, 1.0f});
        InspectionRenderer::assertDisplayed(_, left(cascade5_pixels), float3{1.0f, 0.25f, 0.5f});
        const auto assertGray = [&](const std::array<u8, 4u> &texel, const float linear) {
            const u8 expected = static_cast<u8>(std::lround(inspectionSrgbEncode(linear) * 255.0f));
            PPR_TEST_ASSERT(
                nearByte(texel[0], expected) and nearByte(texel[1], expected) and nearByte(texel[2], expected));
        };
        assertGray(right(cascade1_pixels), 0.5f);
        assertGray(right(cascade5_pixels), 0.25f);

        // Landmark: a translated/zoomed camera at the gameplay 1/3 m cell metric.
        // Native (135,592) is opaque white; its row neighbours outside the map
        // and at the map border row stay distinguishable. A metre-as-cell
        // mapping reads the landmark as (45,197), which is empty.
        const float metric_cells_per_metre = 3.0f;
        const CameraSnapshot metric_camera = inspectionMetricCamera(
            metric_cells_per_metre, float2{-16.0f, 80.0f}, float2{1024.0f, 1024.0f});
        const int2 metric_extent{2048, 2048};
        const auto metric_pixels = renderer.draw(
            inputs, metric_camera, static_cast<u32>(ELightingDiagnostic::occupancy), 1u, metric_extent,
            metric_cells_per_metre);

        const float2 landmark_native{135.5f, 592.5f};
        const float2 landmark_pixel = inspectionPixelAt(
            metric_camera, landmark_native, metric_cells_per_metre, metric_extent);
        const float2 landmark_round_trip = inspectionNativeAt(
            metric_camera, landmark_pixel, metric_cells_per_metre, metric_extent);
        PPR_TEST_ASSERT(std::abs(landmark_round_trip.x - landmark_native.x) <= 1.0e-3f);
        PPR_TEST_ASSERT(std::abs(landmark_round_trip.y - landmark_native.y) <= 1.0e-3f);
        PPR_TEST_ASSERT(static_cast<int>(std::floor(landmark_round_trip.x)) == 135);
        PPR_TEST_ASSERT(static_cast<int>(std::floor(landmark_round_trip.y)) == 592);

        const auto metric_texel = [&](const float2 native) {
            const float2 pixel = inspectionPixelAt(metric_camera, native, metric_cells_per_metre, metric_extent);
            return InspectionRenderer::texel(
                metric_pixels, static_cast<u32>(std::floor(pixel.x)), static_cast<u32>(std::floor(pixel.y)));
        };

        const auto landmark_texel = metric_texel(landmark_native);
        PPR_TEST_ASSERT(
            nearByte(landmark_texel[0], white) and
            nearByte(landmark_texel[1], white) and
            nearByte(landmark_texel[2], white));

        const auto outside_texel = metric_texel(float2{-0.25f, 592.25f});
        PPR_TEST_ASSERT(
            nearByte(outside_texel[0], 0u) and nearByte(outside_texel[1], 0u) and nearByte(outside_texel[2], 0u));

        const auto border_texel = metric_texel(float2{0.25f, 592.25f});
        PPR_TEST_ASSERT(
            nearByte(border_texel[0], white) and nearByte(border_texel[1], white) and nearByte(border_texel[2], white));

        // Read-only decode: no inspected record changed across any view.
        PPR_TEST_ASSERT(
            resolved_before == readBufferBytes(device, *inputs.m_resolved, cell(8u, 8u) * sizeof(u32), sizeof(u32)));
        PPR_TEST_ASSERT(
            source_before == readBufferBytes(device, *inputs.m_source, cell(8u, 8u) * sizeof(u32), sizeof(u32)));
        PPR_TEST_ASSERT(
            fine_before == readBufferBytes(
                               device, *inputs.m_fine_ch, static_cast<u64>(7u * 64u + 9u) * sizeof(lighting::PackedCH),
                               sizeof(lighting::PackedCH)));
        PPR_TEST_ASSERT(
            cascade1_before == readBufferBytes(
                                   device, *inputs.m_cascades,
                                   static_cast<u64>(8u * edge + 136u) * sizeof(lighting::PackedInterval),
                                   sizeof(lighting::PackedInterval)));
        PPR_TEST_ASSERT(
            cascade5_before == readBufferBytes(
                                   device, *inputs.m_cascades,
                                   (static_cast<u64>(4u) * cascade_level_records + static_cast<u64>(8u * edge + 136u)) *
                                       sizeof(lighting::PackedInterval),
                                   sizeof(lighting::PackedInterval)));
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
                detail::WorldLightingMaterial::background_layers_receive_light_at_bounded_offsets,
                detail::WorldLightingMaterial::opaque_thermal_glow_and_receive_only_thermal_exclusion,
                detail::WorldLightingMaterial::inspection_settings_reject_unknown_views_and_levels,
                detail::WorldLightingMaterial::inspection_views_decode_published_records_without_mutation,
            });
        };
        return group;
    }
}
